#include "cursor_clone.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <imgui.h>
#include <string>
#include <array>
#include <thread>
#include <mutex>
#include <future>
#include <unordered_map>
#include <chrono>
#include <nlohmann/json.hpp>

// Add these headers for fcntl and flags
#if !defined(_WIN32)
    #include <fcntl.h>
    #include <unistd.h>
    #include <pty.h>
    #include <termios.h>
    #include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// Define static members
bool CursorClone::needs_refresh = false;
std::string CursorClone::last_directory;
std::chrono::steady_clock::time_point CursorClone::last_refresh_time = std::chrono::steady_clock::now();

// Add these color constants at the top of the file or in a suitable location
namespace TerminalColors {
    const ImVec4 Default = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);    // Light gray
    const ImVec4 Command = ImVec4(0.55f, 0.79f, 0.94f, 1.0f);    // Light blue
    const ImVec4 Error = ImVec4(0.94f, 0.37f, 0.37f, 1.0f);      // Red
    const ImVec4 Success = ImVec4(0.65f, 0.85f, 0.35f, 1.0f);    // Green
    const ImVec4 Warning = ImVec4(0.94f, 0.79f, 0.37f, 1.0f);    // Yellow
    const ImVec4 Path = ImVec4(0.37f, 0.79f, 0.94f, 1.0f);       // Blue
    const ImVec4 Prompt = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);     // Green
}

CursorClone::CursorClone(const std::string& api_key) 
    : groq_client(api_key) {
    
    // Get Pinecone API key from environment
    const char* pinecone_key = std::getenv("PINECONE_API_KEY");
    const char* pinecone_env = std::getenv("PINECONE_ENVIRONMENT");
    
    if (!pinecone_key || !pinecone_env) {
        terminal_history.push_back("Warning: PINECONE_API_KEY and/or PINECONE_ENVIRONMENT not set. Directory embeddings will be disabled.");
        embeddings_enabled = false;
    } else {
        try {
            pinecone_client = std::make_unique<PineconeClient>(
                std::string(pinecone_key),
                std::string(pinecone_env),
                groq_client
            );
            embeddings_enabled = true;
            terminal_history.push_back("Successfully initialized Pinecone client.");
        } catch (const std::exception& e) {
            terminal_history.push_back("Warning: Failed to initialize Pinecone client: " + std::string(e.what()));
            terminal_history.push_back("Directory embeddings will be disabled.");
            embeddings_enabled = false;
        }
    }
    
    // Initialize chat history with a welcome message
    chat_history = "Welcome to Cursor Clone! How can I help you today?\n";
    
    // Initialize input buffers
    input_buffer[0] = '\0';
    terminal_buffer[0] = '\0';
    file_path_buffer[0] = '\0';
    
    // Set up current directory
    current_directory = fs::current_path().string();
    loadDirectoryQuick(current_directory);
    
    // Set up fonts
    setupFonts();
}

CursorClone::~CursorClone() {
    cancelCurrentLoading();
}

void CursorClone::refreshDirectoryContent() {
    static std::unordered_map<std::string, std::vector<fs::directory_entry>> directory_cache;
    static const size_t MAX_CACHE_SIZE = 10;
    
    // Try to find in cache first
    auto it = directory_cache.find(current_directory);
    if (it != directory_cache.end()) {
        current_directory_files = it->second;
        return;
    }
    
    // Load directory contents
    loadDirectoryQuick(current_directory);
    
    // Cache the results
    directory_cache[current_directory] = current_directory_files;
    
    // Clear cache if it gets too large
    if (directory_cache.size() > MAX_CACHE_SIZE) {
        directory_cache.clear();
    }
}

void CursorClone::cancelCurrentLoading() {
    if (directory_loader.valid()) {
        cancel_loading = true;
        directory_loader.wait();
        cancel_loading = false;
    }
}

void CursorClone::loadDirectoryAsync(const std::string& path) {
    try {
        std::vector<fs::directory_entry> entries;
        entries.reserve(100);  // Smaller initial reserve
        
        fs::path dir_path = fs::absolute(path);
        
        // Only store the most important file types
        static const std::unordered_set<std::string> quick_extensions = {
            ".cpp", ".hpp", ".h", ".py", ".txt"
        };
        
        // Single pass, minimal checks
        for (const auto& entry : fs::directory_iterator(dir_path)) {
            if (entry.is_directory()) {
                entries.push_back(entry);
                continue;
            }
            
            std::string ext = entry.path().extension().string();
            if (quick_extensions.find(ext) != quick_extensions.end()) {
                entries.push_back(entry);
            }
        }

        // Simple sort: directories first, then alphabetical
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) {
                bool a_is_dir = a.is_directory();
                bool b_is_dir = b.is_directory();
                return a_is_dir > b_is_dir || 
                       (a_is_dir == b_is_dir && 
                        a.path().filename().string() < b.path().filename().string());
            });

        current_directory_files = std::move(entries);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading directory: " << e.what() << std::endl;
    }
}

void CursorClone::clearOldCache() {
    if (directory_cache.size() > MAX_CACHE_SIZE) {
        directory_cache.clear();
    }
}

bool CursorClone::isRecognizedFileType(const std::string& ext) {
    static const std::unordered_set<std::string> recognized_extensions = {
        ".cpp", ".hpp", ".h", ".c", ".py", ".txt", ".json", ".md", ".cmake"
    };
    return recognized_extensions.find(ext) != recognized_extensions.end();
}

std::string CursorClone::getFileIcon(const fs::directory_entry& entry) {
    if (entry.is_directory()) return "[DIR]";
    
    static const std::unordered_map<std::string, const char*> icons = {
        {".cpp", "[C++]"},
        {".hpp", "[C++]"},
        {".h", "[H]"},
        {".py", "[PY]"},
        {".txt", "[TXT]"}
    };
    
    std::string ext = entry.path().extension().string();
    auto it = icons.find(ext);
    return it != icons.end() ? it->second : "[F]";
}

std::string CursorClone::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "Error: Could not open file";
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return content;
}

void CursorClone::writeFile(const std::string& path, const std::string& content) {
    try {
        fs::path file_path(path);
        if (file_path.has_parent_path()) {
            fs::create_directories(file_path.parent_path());
        }
        
        std::string cleaned_content = content;
        
        // Remove all XML/HTML-like tags
        size_t tag_start;
        while ((tag_start = cleaned_content.find('<')) != std::string::npos) {
            size_t tag_end = cleaned_content.find('>', tag_start);
            if (tag_end != std::string::npos) {
                cleaned_content.erase(tag_start, tag_end - tag_start + 1);
            } else {
                break;
            }
        }

        // Process content line by line
        std::istringstream stream(cleaned_content);
        std::string line;
        std::vector<std::string> lines;
        
        while (std::getline(stream, line)) {
            // Skip empty lines or lines with only special characters
            if (line.find_first_not_of(" \t\r\n`\"'<>") == std::string::npos) {
                continue;
            }
            
            // Remove any remaining tags or special characters
            std::string clean_line;
            bool in_tag = false;
            
            for (char c : line) {
                if (c == '<') {
                    in_tag = true;
                    continue;
                }
                if (c == '>') {
                    in_tag = false;
                    continue;
                }
                if (!in_tag && c != '`' && c != '\r') {
                    clean_line += c;
                }
            }
            
            // Remove quotes at start/end
            while (clean_line.size() >= 2 && 
                   ((clean_line.front() == '"' && clean_line.back() == '"') ||
                    (clean_line.front() == '\'' && clean_line.back() == '\'') ||
                    (clean_line.front() == '`' && clean_line.back() == '`'))) {
                clean_line = clean_line.substr(1, clean_line.size() - 2);
            }
            
            // Only add non-empty lines
            if (!clean_line.empty() && 
                clean_line.find_first_not_of(" \t") != std::string::npos) {
                lines.push_back(clean_line);
            }
        }

        // Join lines with proper line endings
        std::string final_content;
        for (size_t i = 0; i < lines.size(); ++i) {
            final_content += lines[i];
            if (i < lines.size() - 1) {
                final_content += '\n';
            }
        }

        // Ensure final newline
        if (!final_content.empty() && final_content.back() != '\n') {
            final_content += '\n';
        }
        
        // Handle Python-specific requirements
        bool isNewPythonFile = (file_path.extension() == ".py" && !fs::exists(file_path));
        
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file for writing: " + path);
        }
        
        if (isNewPythonFile) {
            if (final_content.find("#!/usr/bin/env python") == std::string::npos) {
                file << "#!/usr/bin/env python3\n";
            }
            if (final_content.find("# -*- coding:") == std::string::npos) {
                file << "# -*- coding: utf-8 -*-\n\n";
            }
        }
        
        file.write(final_content.c_str(), final_content.length());
        file.close();
        
        #if !defined(_WIN32)
        if (file_path.extension() == ".py") {
            fs::permissions(file_path, 
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                fs::perm_options::add);
        }
        #endif
        
        // After successfully writing the file, force a directory refresh
        if (fs::path(path).parent_path() == fs::path(current_directory)) {
            forceDirectoryRefresh();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error writing file " << path << ": " << e.what() << std::endl;
        throw;
    }
}

std::string CursorClone::getAIAssistance(const std::string& query) {
    try {
        // Create input context string with safety checks
        std::string context = "You are an AI programming assistant. You can run code directly in the terminal.\n\n";
        context += "Important: When showing commands, use ```bash blocks and write ONLY the exact commands to run.\n";
        context += "When showing Python code, use ```python blocks.\n";
        context += "Current working directory: " + current_directory + "\n\n";
        
        // Add directory listing
        context += "Files in current directory:\n";
        for (const auto& entry : current_directory_files) {
            context += (entry.is_directory() ? "[DIR] " : "[FILE] ") + 
                      entry.path().filename().string() + "\n";
        }
        
        // Add editor context
        context += "\n" + std::string(editor.isFileOpen() ? 
            "Current file: " + editor.getCurrentFile() : 
            "No file is currently open.");

        std::string response = groq_client.getCompletion(context + "\n\n" + query);
        
        // Add a simple separator
        terminal_history.push_back("\n--- AI Assistant Response ---");
        
        // Process code blocks and execute them
        size_t pos = 0;
        bool first_command = true;
        while ((pos = response.find("```", pos)) != std::string::npos) {
            // Find the block type
            size_t type_end = response.find('\n', pos);
            if (type_end == std::string::npos) break;
            
            std::string block_type = response.substr(pos + 3, type_end - (pos + 3));
            // Remove any whitespace or extra characters
            block_type = block_type.substr(0, block_type.find_first_of(" \t\r\n"));
            
            // Find the end of the code block
            size_t start = type_end + 1;
            size_t end = response.find("```", start);
            if (end == std::string::npos) break;
            
            // Extract and clean the code/command
            std::string content = response.substr(start, end - start);
            // Trim whitespace and newlines
            while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
                content.pop_back();
            }
            while (!content.empty() && (content.front() == '\n' || content.front() == '\r')) {
                content.erase(0, 1);
            }
            
            if (!content.empty()) {
                if (block_type == "python") {
                    // If not the first command, add a delay
                    if (!first_command) {
                        terminal_history.push_back("Waiting before next command...");
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                    
                    // Generate a unique filename for the Python script
                    std::string filename = "ai_script_" + 
                        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + 
                        ".py";
                    
                    terminal_history.push_back("Running Python script: " + filename);
                    // Execute the Python code
                    executeAIGeneratedCode(content, filename);
                    
                } else if (block_type == "bash") {
                    // Split and execute each command line
                    std::istringstream command_stream(content);
                    std::string command;
                    bool first_line = true;
                    
                    while (std::getline(command_stream, command)) {
                        // Trim whitespace
                        command.erase(0, command.find_first_not_of(" \t\r\n"));
                        command.erase(command.find_last_not_of(" \t\r\n") + 1);
                        
                        if (!command.empty()) {
                            // If not the first command overall or first line, add a delay
                            if (!first_command || !first_line) {
                                terminal_history.push_back("Waiting before next command...");
                                std::this_thread::sleep_for(std::chrono::seconds(2));
                            }
                            
                            terminal_history.push_back("$ " + command);
                            executeCommandAsync(command);
                            
                            // Wait for the command to complete
                            while (async_command && async_command->running) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                checkCommandOutput();
                            }
                            
                            first_line = false;
                        }
                    }
                }
                first_command = false;
            }
            
            pos = end + 3;
        }
        
        terminal_history.push_back("--- End of AI Response ---\n");
        return response;
        
    } catch (const std::exception& e) {
        terminal_history.push_back("Error: " + std::string(e.what()));
        return "Error: " + std::string(e.what());
    }
}

void CursorClone::renderGUI() {
    // Set up the window to be fullscreen with no padding
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    
    // Use a consistent background color and style
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | 
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus;

    // Begin the main window
    ImGui::Begin("MainWindow", nullptr, window_flags);
    
    float width = ImGui::GetWindowWidth();
    float height = ImGui::GetWindowHeight();

    // Left panel (File Manager) with default font
    ImGui::PushFont(defaultFont);
    ImGui::BeginChild("FileManager", ImVec2(width * 0.2f, -1), true);
    
    // Add browse button at the top
    showBrowseButton();
    ImGui::SameLine();
    
    // Show current path
    ImGui::Text("Path: %s", getDisplayPath(current_directory).c_str());
    ImGui::Separator();

    // Check if we need to refresh
    static auto last_check_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    auto time_since_check = std::chrono::duration_cast<std::chrono::milliseconds>(
        current_time - last_check_time).count();

    // Refresh directory listing periodically and when needed
    if (needs_refresh || time_since_check > 500) {  // Check every 500ms
        loadDirectoryQuick(current_directory);
        needs_refresh = false;
        last_check_time = current_time;
    }

    // File list
    ImGui::BeginChild("FileList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    
    // Use clipper for efficient rendering
    static ImGuiListClipper clipper;
    clipper.Begin(current_directory_files.size());
    
    // Pre-allocate label buffer
    static char label[256];
    static std::string icon_cache;
    
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            if (i >= current_directory_files.size()) break;
            
            const auto& entry = current_directory_files[i];
            bool is_dir = entry.is_directory();
            
            // Fast string formatting without allocations
            const char* icon = is_dir ? "[D] " : "[F] ";
            snprintf(label, sizeof(label), "%s%s", icon, 
                    entry.path().filename().string().c_str());
            
            if (ImGui::Selectable(label, false)) {
                if (is_dir) {
                    changeDirectory(entry.path().string());
                } else {
                    editor.openFile(entry.path().string());
                }
            }
        }
    }

    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopFont();

    ImGui::SameLine();

    // Middle panel with mono font
    ImGui::PushFont(monoFont);
    ImGui::BeginChild("EditorPanel", ImVec2(width * 0.5f, -1), true);

    // Editor takes up 70% of the panel height
    float editor_height = ImGui::GetContentRegionAvail().y * 0.7f;
    ImGui::BeginChild("Editor", ImVec2(0, editor_height), true);
    editor.render(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    // Terminal takes up the remaining height
    ImGui::BeginChild("Terminal", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
    {
        // Show current directory at the top of terminal
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", getDisplayPath(current_directory).c_str());
        ImGui::Separator();

        checkCommandOutput();

        // Calculate content area
        const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false, ImGuiWindowFlags_HorizontalScrollbar);

        // Terminal output with wrapping
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
        float wrap_width = ImGui::GetContentRegionAvail().x;

        // Display history
        for (const auto& line : terminal_history) {
            ImVec4 color = TerminalColors::Default;
            
            // Color coding logic...
            if (line.length() >= 2 && line[0] == '>' && line[1] == ' ') {
                color = TerminalColors::Command;
            } else if (line.find("Error:") != std::string::npos || 
                       line.find("error:") != std::string::npos ||
                       line.find("failed") != std::string::npos) {
                color = TerminalColors::Error;
            } else if (line.find("Warning:") != std::string::npos || 
                       line.find("warning:") != std::string::npos) {
                color = TerminalColors::Warning;
            } else if (line.find("Success") != std::string::npos || 
                       line.find("successfully") != std::string::npos) {
                color = TerminalColors::Success;
            } else if (line.find("/") != std::string::npos || 
                       line.find("\\") != std::string::npos) {
                color = TerminalColors::Path;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
        }

        // Show prompt and input field
        if (!async_command || !async_command->running || async_command->waiting_for_input) {
            // Show appropriate prompt based on state
            if (async_command && async_command->waiting_for_input) {
                ImGui::TextColored(TerminalColors::Prompt, "> ");
            } else {
                std::string prompt = getUsername() + "@" + getHostname() + ":" + getDisplayPath(current_directory) + "$ ";
                ImGui::TextColored(TerminalColors::Prompt, "%s", prompt.c_str());
            }
            
            ImGui::SameLine();
            
            // Input field styling
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            
            // Calculate remaining width for input
            float remaining_width = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remaining_width);
            
            // Handle input
            bool input_received = ImGui::InputText("##TerminalInput", 
                terminal_buffer, 
                sizeof(terminal_buffer),
                ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_CallbackHistory |
                ImGuiInputTextFlags_CallbackCompletion,
                terminalInputCallback,
                this
            );

            if (input_received && terminal_buffer[0] != '\0') {
                std::string input(terminal_buffer);
                
                if (async_command && async_command->waiting_for_input) {
                    // Send input to running process
                    sendInput(input + "\n");
                    terminal_history.push_back("> " + input);
                } else {
                    // Execute as new command
                    executeCommandAsync(input);
                    command_history.push_back(input);
                    if (command_history.size() > MAX_COMMAND_HISTORY) {
                        command_history.erase(command_history.begin());
                    }
                }
                
                terminal_buffer[0] = '\0';  // Clear input buffer
            }
            
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            
            // Auto-focus the input field
            if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive()) {
                ImGui::SetKeyboardFocusHere(-1);
            }
        }

        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopFont();

    ImGui::SameLine();

    // Right panel (AI Assistant) with default font
    ImGui::PushFont(defaultFont);
    ImGui::BeginChild("Assistant", ImVec2(0, -1), true);
    
    // Chat history
    float chatHistoryHeight = -ImGui::GetFrameHeightWithSpacing() * 2;
    ImGui::BeginChild("ChatHistory", ImVec2(0, chatHistoryHeight), true);
    {
        if (!chat_history.empty()) {
            try {
                // Display in chunks to prevent buffer overflow
                const size_t CHUNK_SIZE = 1024;
                for (size_t i = 0; i < chat_history.length(); i += CHUNK_SIZE) {
                    std::string chunk = chat_history.substr(i, CHUNK_SIZE);
                    ImGui::TextWrapped("%s", chunk.c_str());
                }
            } catch (const std::exception& e) {
                ImGui::TextWrapped("Error displaying chat history: %s", e.what());
            }
        }
        
        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    // Input area with improved handling
    ImGui::PushItemWidth(-1); // Make input field fill the width
    bool input_submitted = ImGui::InputText("##AIInput", input_buffer, sizeof(input_buffer), 
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    if (ImGui::Button("Send") || input_submitted) {
        if (strlen(input_buffer) > 0) {
            try {
                std::string query(input_buffer);
                
                // Clear input buffer immediately
                input_buffer[0] = '\0';
                
                // Safely append user query
                std::string new_message = "\n\nYou: " + query;
                if (new_message.length() > MAX_MESSAGE_SIZE) {
                    new_message = new_message.substr(0, MAX_MESSAGE_SIZE) + "... (truncated)";
                }
                
                // Ensure we have space in chat history
                if (chat_history.length() + new_message.length() > MAX_CHAT_HISTORY) {
                    // Remove old messages until we have space
                    size_t remove_pos = chat_history.find("\n\n", chat_history.length() / 2);
                    if (remove_pos != std::string::npos) {
                        chat_history = chat_history.substr(remove_pos + 2);
                    } else {
                        chat_history.clear();
                    }
                }
                
                chat_history += new_message;
                
                // Get AI response
                std::string response = getAIAssistance(query);
                
                // Safely append AI response
                if (!response.empty()) {
                    std::string ai_message = "\n\nAssistant: " + response;
                    if (ai_message.length() > MAX_MESSAGE_SIZE) {
                        ai_message = ai_message.substr(0, MAX_MESSAGE_SIZE) + "... (truncated)";
                    }
                    
                    // Check space again for AI response
                    if (chat_history.length() + ai_message.length() > MAX_CHAT_HISTORY) {
                        size_t remove_pos = chat_history.find("\n\n", chat_history.length() / 2);
                        if (remove_pos != std::string::npos) {
                            chat_history = chat_history.substr(remove_pos + 2);
                        } else {
                            chat_history.clear();
                        }
                    }
                    
                    chat_history += ai_message;
                }
                
            } catch (const std::exception& e) {
                std::cerr << "Error processing chat: " << e.what() << std::endl;
                chat_history += "\n\nError: Failed to process request - " + std::string(e.what());
            }
        }
    }
    
    ImGui::EndChild();
    ImGui::PopFont();
    
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Handle keyboard shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_LeftAlt) && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        goBack();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftAlt) && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        goForward();
    }

    // Add back/forward buttons to the file manager
    if (ImGui::Button("←")) {
        goBack();
    }
    ImGui::SameLine();
    if (ImGui::Button("→")) {
        goForward();
    }
}

void CursorClone::setupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Get system font paths based on OS
    std::string systemFont, monospacedFont;
    
    #if defined(_WIN32)
        systemFont = "C:\\Windows\\Fonts\\segoeui.ttf";
        monospacedFont = "C:\\Windows\\Fonts\\consola.ttf";
    #elif defined(__APPLE__)
        systemFont = "/System/Library/Fonts/SFNSText.ttf";
        monospacedFont = "/System/Library/Fonts/SFMono-Regular.ttf";
    #else // Linux
        // Common Linux system font locations (prioritizing regular fonts over emoji)
        std::array<std::string, 8> possibleSystemFonts = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/ubuntu/Ubuntu-R.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf"
        };
        
        std::array<std::string, 6> possibleMonoFonts = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
            "/usr/share/fonts/ubuntu-mono/UbuntuMono-R.ttf",
            "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
        };
        
        // Find first existing system font
        bool foundSystemFont = false;
        for (const auto& font : possibleSystemFonts) {
            if (std::filesystem::exists(font)) {
                std::cout << "Found system font: " << font << std::endl;
                systemFont = font;
                foundSystemFont = true;
                break;
            }
        }
        
        // Find first existing mono font
        bool foundMonoFont = false;
        for (const auto& font : possibleMonoFonts) {
            if (std::filesystem::exists(font)) {
                std::cout << "Found mono font: " << font << std::endl;
                monospacedFont = font;
                foundMonoFont = true;
                break;
            }
        }
        
        if (!foundSystemFont || !foundMonoFont) {
            std::cerr << "Warning: Could not find some fonts. Using defaults." << std::endl;
        }
    #endif
    
    // Start with default font
    defaultFont = io.Fonts->AddFontDefault();
    monoFont = io.Fonts->AddFontDefault();
    
    // Basic font configuration
    ImFontConfig config;
    config.MergeMode = false;
    config.PixelSnapH = true;
    
    // Load system font if found
    if (!systemFont.empty()) {
        ImFont* newFont = io.Fonts->AddFontFromFileTTF(systemFont.c_str(), 16.0f, &config);
        if (newFont != nullptr) {
            defaultFont = newFont;
        }
    }
    
    // Load monospace font if found
    if (!monospacedFont.empty()) {
        ImFont* newFont = io.Fonts->AddFontFromFileTTF(monospacedFont.c_str(), 16.0f, &config);
        if (newFont != nullptr) {
            monoFont = newFont;
        }
    }
    
    // Build font atlas
    io.Fonts->Build();
}

void CursorClone::navigateToParentDirectory() {
    try {
        fs::path current(current_directory);
        fs::path parent = current.parent_path();
        
        if (!parent.empty() && parent != current) {
            // Store current directory in history before changing
            if (directory_history.size() >= MAX_DIRECTORY_HISTORY) {
                directory_history.erase(directory_history.begin());
            }
            directory_history.push_back(current_directory);
            current_history_index = directory_history.size();
            
            changeDirectory(parent.string());
        }
    } catch (const std::exception& e) {
        terminal_history.push_back("Error navigating to parent: " + std::string(e.what()));
    }
}

void CursorClone::goBack() {
    if (current_history_index > 0) {
        current_history_index--;
        changeDirectory(directory_history[current_history_index]);
    }
}

void CursorClone::goForward() {
    if (current_history_index < directory_history.size() - 1) {
        current_history_index++;
        changeDirectory(directory_history[current_history_index]);
    }
}

void CursorClone::changeDirectory(const std::string& new_path) {
    try {
        if (new_path.empty()) {
            terminal_history.push_back("Error: Empty path provided");
            return;
        }

        fs::path path;
        try {
            // Handle special cases
            if (new_path == "~") {
                path = getHomeDirectory();
            } else if (new_path.length() >= 2 && new_path.substr(0, 2) == "~/") {
                path = fs::path(getHomeDirectory()) / new_path.substr(2);
            } else if (new_path == "-") {
                // Go back to previous directory
                if (!directory_history.empty()) {
                    path = directory_history.back();
                } else {
                    terminal_history.push_back("Error: No previous directory");
                    return;
                }
            } else {
                // Handle relative and absolute paths
                path = fs::absolute(fs::path(current_directory) / new_path);
            }
        } catch (const std::exception& e) {
            terminal_history.push_back("Error resolving path: " + std::string(e.what()));
            return;
        }

        // Check if path exists and is a directory
        if (!fs::exists(path)) {
            terminal_history.push_back("Error: Directory does not exist: " + path.string());
            return;
        }
        if (!fs::is_directory(path)) {
            terminal_history.push_back("Error: Not a directory: " + path.string());
            return;
        }

        // Store current directory in history before changing
        if (current_directory != path.string()) {
            directory_history.push_back(current_directory);
            if (directory_history.size() > MAX_DIRECTORY_HISTORY) {
                directory_history.erase(directory_history.begin());
            }
        }

        // Change directory
        current_directory = path.string();
        if (chdir(current_directory.c_str()) != 0) {
            terminal_history.push_back("Warning: Failed to change process directory: " + std::string(strerror(errno)));
        }

        // Update directory listing
        loadDirectoryQuick(current_directory);
        terminal_history.push_back("Changed to: " + getDisplayPath(current_directory));

    } catch (const std::exception& e) {
        terminal_history.push_back("Error: " + std::string(e.what()));
    }
}

void CursorClone::showDirectoryContextMenu(const fs::path& path) {
    // Add parent directory button
    if (ImGui::Button("..")) {
        navigateToParentDirectory();
    }

    // Add "New" button
    if (ImGui::Button("New")) {
        ImGui::OpenPopup("NewItemPopup");
    }

    // New item popup
    if (ImGui::BeginPopup("NewItemPopup")) {
        if (ImGui::MenuItem("New File")) {
            ImGui::OpenPopup("NewFilePopup");
        }
        if (ImGui::MenuItem("New Directory")) {
            ImGui::OpenPopup("NewDirPopup");
        }
        ImGui::EndPopup();
    }

    // New file popup
    static char new_file_name[256] = "";
    if (ImGui::BeginPopupModal("NewFilePopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter file name:");
        ImGui::InputText("##filename", new_file_name, sizeof(new_file_name));
        
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            try {
                fs::path new_file_path = fs::path(current_directory) / new_file_name;
                std::ofstream file(new_file_path);
                if (file.is_open()) {
                    file.close();
                    editor.openFile(new_file_path.string());
                    
                    // Force immediate refresh
                    forceDirectoryRefresh();
                    loadDirectoryQuick(current_directory);
                }
                new_file_name[0] = '\0';  // Clear the input
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& e) {
                std::cerr << "Error creating file: " << e.what() << std::endl;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            new_file_name[0] = '\0';  // Clear the input
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New directory popup
    static char new_dir_name[256] = "";
    if (ImGui::BeginPopupModal("NewDirPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter directory name:");
        ImGui::InputText("##dirname", new_dir_name, sizeof(new_dir_name));
        
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            try {
                fs::path new_dir_path = fs::path(current_directory) / new_dir_name;
                fs::create_directory(new_dir_path);
                
                // Force immediate refresh
                forceDirectoryRefresh();
                loadDirectoryQuick(current_directory);
                
                new_dir_name[0] = '\0';  // Clear the input
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& e) {
                std::cerr << "Error creating directory: " << e.what() << std::endl;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            new_dir_name[0] = '\0';  // Clear the input
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Context menu for right-click
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Copy Path")) {
            ImGui::SetClipboardText(path.string().c_str());
        }
        if (ImGui::MenuItem("Open in Terminal")) {
            // Open terminal in this directory (OS-specific)
            #if defined(_WIN32)
                std::string cmd = "start cmd /K \"cd /d " + path.string() + "\"";
            #else
                std::string cmd = "x-terminal-emulator --working-directory=\"" + path.string() + "\" &";
            #endif
            system(cmd.c_str());
        }
        ImGui::EndPopup();
    }
}

void CursorClone::openSystemDirectoryBrowser() {
    #ifdef _WIN32
        // Windows implementation using modern Common Item Dialog
        #include <windows.h>
        #include <shobjidl.h> 
        
        // Initialize COM
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr))
            return;

        IFileOpenDialog *pFileOpen;
        hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, 
                            IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));

        if (SUCCEEDED(hr)) {
            // Set options to pick folders
            FILEOPENDIALOGOPTIONS options;
            pFileOpen->GetOptions(&options);
            pFileOpen->SetOptions(options | FOS_PICKFOLDERS);
            
            // Show the dialog
            hr = pFileOpen->Show(NULL);

            if (SUCCEEDED(hr)) {
                IShellItem *pItem;
                hr = pFileOpen->GetResult(&pItem);
                if (SUCCEEDED(hr)) {
                    PWSTR pszFilePath;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr)) {
                        // Convert wide string to regular string and change directory
                        std::wstring ws(pszFilePath);
                        std::string path(ws.begin(), ws.end());
                        changeDirectory(path);
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
            pFileOpen->Release();
        }
        CoUninitialize();
        
    #elif defined(__APPLE__)
        // macOS implementation using NSOpenPanel via AppleScript
        std::string command = "osascript -e 'tell application \"System Events\"' "
                            "-e 'activate' "
                            "-e 'set folderPath to choose folder with prompt \"Select Directory\"' "
                            "-e 'POSIX path of folderPath' "
                            "-e 'end tell'";
                            
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe) {
            char buffer[1024];
            std::string result;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result += buffer;
            }
            pclose(pipe);
            
            // Clean up the result (remove newlines and quotes)
            if (!result.empty()) {
                result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
                result.erase(std::remove(result.begin(), result.end(), '"'), result.end());
                if (!result.empty()) {
                    changeDirectory(result);
                }
            }
        }
        
    #else
        // Linux implementation using native dialog (zenity/kdialog/qarma)
        std::string dialog_cmd;
        
        // Try to detect the desktop environment
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        const char* session = std::getenv("DESKTOP_SESSION");
        
        if (desktop && strcasestr(desktop, "KDE") != nullptr) {
            // KDE - use kdialog
            dialog_cmd = "kdialog --getexistingdirectory .";
        } else if (system("which zenity >/dev/null 2>&1") == 0) {
            // GNOME/Unity/Others with zenity
            dialog_cmd = "zenity --file-selection --directory";
        } else if (system("which qarma >/dev/null 2>&1") == 0) {
            // Fallback to qarma (Qt clone of zenity)
            dialog_cmd = "qarma --file-selection --directory";
        } else if (system("which yad >/dev/null 2>&1") == 0) {
            // Fallback to yad (another zenity alternative)
            dialog_cmd = "yad --file --directory";
        } else {
            // If no GUI dialog is available, use terminal-based dialog
            dialog_cmd = "dialog --stdout --title \"Select Directory\" --dselect . 0 0";
        }
        
        FILE* pipe = popen(dialog_cmd.c_str(), "r");
        if (!pipe) {
            std::cerr << "Error opening directory browser" << std::endl;
            return;
        }

        char buffer[1024];
        std::string result;
        
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        
        pclose(pipe);
        
        // Clean up the result
        if (!result.empty()) {
            // Remove trailing newline if present
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
            
            if (!result.empty()) {
                changeDirectory(result);
            }
        }
    #endif
}

int CursorClone::terminalInputCallback(ImGuiInputTextCallbackData* data) {
    CursorClone* terminal = static_cast<CursorClone*>(data->UserData);
    return terminal->handleTerminalInput(data);
}

bool CursorClone::handleTerminalInput(ImGuiInputTextCallbackData* data) {
    switch (data->EventFlag) {
        case ImGuiInputTextFlags_CallbackHistory: {
            // Handle up/down arrows for command history
            navigateCommandHistory(data->EventKey == ImGuiKey_UpArrow);
            return true;
        }
        case ImGuiInputTextFlags_CallbackCompletion: {
            // Handle tab completion
            std::string current(data->Buf);
            if (current.empty()) return false;

            // Get all files in current directory for completion
            std::vector<std::string> matches;
            for (const auto& entry : current_directory_files) {
                std::string name = entry.path().filename().string();
                if (name.substr(0, current.length()) == current) {
                    matches.push_back(name);
                }
            }

            if (matches.size() == 1) {
                // Single match - complete it
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, matches[0].c_str());
            } else if (matches.size() > 1) {
                // Multiple matches - show them
                terminal_history.push_back("Possible completions:");
                for (const auto& match : matches) {
                    terminal_history.push_back("  " + match);
                }
            }
            return true;
        }
    }
    return false;
}

void CursorClone::navigateCommandHistory(bool up) {
    if (command_history.empty()) return;

    if (up) {
        if (command_history_index < command_history.size()) {
            command_history_index++;
            strcpy(terminal_buffer, command_history[command_history.size() - command_history_index].c_str());
        }
    } else {
        if (command_history_index > 1) {
            command_history_index--;
            strcpy(terminal_buffer, command_history[command_history.size() - command_history_index].c_str());
        } else if (command_history_index == 1) {
            command_history_index = 0;
            terminal_buffer[0] = '\0';
        }
    }
}

void CursorClone::executeTerminalCommand(const std::string& command) {
    try {
        command_history.push_back(command);
        if (command_history.size() > MAX_COMMAND_HISTORY) {
            command_history.erase(command_history.begin());
        }
        command_history_index = 0;

        // Handle built-in commands
        if (command == "clear" || command == "cls") {
            terminal_history.clear();
            return;
        }

        // For AI-generated code execution, don't add the normal prompt
        if (terminal_history.size() >= 2 && 
            terminal_history[terminal_history.size() - 2].find("Script Output") != std::string::npos) {
            // We're inside the script output box, don't add extra formatting
            executeCommandAsync(command);
        } else {
            // Normal command execution
            std::string prompt = getUsername() + "@" + getHostname() + ":" + getDisplayPath(current_directory) + "$ ";
            terminal_history.push_back(prompt + command);
            executeCommandAsync(command);
        }

    } catch (const std::exception& e) {
        terminal_history.push_back("Error: " + std::string(e.what()));
    }
}

void CursorClone::executeCommandAsync(const std::string& command) {
    // Handle special commands
    if (command == "cd -") {
        goBack();
        return;
    }

    // Handle cd command specially
    if (command.substr(0, 3) == "cd ") {
        std::string path = command.substr(3);
        // Trim whitespace
        path.erase(0, path.find_first_not_of(" \t\r\n"));
        path.erase(path.find_last_not_of(" \t\r\n") + 1);
        
        try {
            changeDirectory(path);
        } catch (const std::exception& e) {
            terminal_history.push_back("Error: " + std::string(e.what()));
        }
        return;
    }

    // Rest of executeCommandAsync implementation for other commands...
    cancelCurrentLoading();  // Cancel any existing command
    
    async_command = std::make_unique<AsyncCommand>();
    async_command->command = command;
    async_command->running = true;
    async_command->command_running = true;  // Add this line
    
    // Create pipe for input
    if (pipe(async_command->input_pipe) == -1) {
        terminal_history.push_back("Error: Failed to create input pipe");
        return;
    }

    // Create pseudo-terminal
    async_command->master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (async_command->master_fd == -1) {
        terminal_history.push_back("Error: Failed to create pseudo-terminal");
        return;
    }

    grantpt(async_command->master_fd);
    unlockpt(async_command->master_fd);

    async_command->future = std::async(std::launch::async, [this, command]() {
        // First change to the correct directory
        if (chdir(current_directory.c_str()) != 0) {
            std::string error = "Failed to change directory: " + std::string(strerror(errno));
            terminal_history.push_back(error);
            return;
        }

        pid_t pid = fork();
        
        if (pid == 0) {  // Child process
            // Set up slave end of PTY
            int slave_fd = open(ptsname(async_command->master_fd), O_RDWR);
            
            // Redirect stdin/stdout/stderr
            dup2(slave_fd, STDIN_FILENO);
            dup2(slave_fd, STDOUT_FILENO);
            dup2(slave_fd, STDERR_FILENO);
            
            // Connect input pipe to stdin
            dup2(async_command->input_pipe[0], STDIN_FILENO);
            
            close(async_command->master_fd);
            close(async_command->input_pipe[0]);
            close(async_command->input_pipe[1]);
            
            // Set up environment
            setenv("TERM", "xterm-256color", 1);
            setenv("PYTHONUNBUFFERED", "1", 1);
            setenv("PYTHONIOENCODING", "utf-8", 1);
            
            // Execute the command
            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            perror("execl failed");
            exit(1);
        }
        
        // Parent process
        char buffer[1024];
        fd_set read_fds;
        
        while (async_command && async_command->running) {
            FD_ZERO(&read_fds);
            FD_SET(async_command->master_fd, &read_fds);
            
            struct timeval tv = {0, 100000};  // 100ms timeout
            
            int ret = select(async_command->master_fd + 1, &read_fds, nullptr, nullptr, &tv);
            
            if (ret > 0 && FD_ISSET(async_command->master_fd, &read_fds)) {
                ssize_t n = read(async_command->master_fd, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    buffer[n] = '\0';
                    std::string output(buffer);
                    
                    // Check for common input prompts
                    if (output.find("input(") != std::string::npos ||
                        output.find("Input") != std::string::npos ||
                        output.find("Enter") != std::string::npos ||
                        output.find(": ") != std::string::npos ||
                        output.find(">>> ") != std::string::npos ||  // Python prompt
                        output.find("... ") != std::string::npos) {
                        async_command->waiting_for_input = true;
                    }
                    
                    std::lock_guard<std::mutex> lock(async_command->output_mutex);
                    async_command->output_buffer.push_back(output);
                }
            }

            // Check if process has terminated
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                async_command->running = false;
                async_command->command_running = false;
                break;
            }
        }
    });
}

void CursorClone::checkCommandOutput() {
    if (!async_command) return;

    // Process any pending output
    {
        std::lock_guard<std::mutex> lock(async_command->output_mutex);
        while (!async_command->output_buffer.empty()) {
            std::string output = async_command->output_buffer.front();
            async_command->output_buffer.pop_front();
            
            // Split output into lines and add to terminal history
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                // Remove carriage returns and other control characters
                line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
                if (!line.empty()) {
                    terminal_history.push_back(line);
                }
            }
        }
    }

    // Check if command has finished
    if (async_command->future.valid() && 
        async_command->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        
        if (!async_command->waiting_for_input) {
            async_command->running = false;
            async_command->command_running = false;
        }
    }

    // Limit terminal history size
    while (terminal_history.size() > MAX_TERMINAL_LINES) {
        terminal_history.pop_front();
    }
}

static constexpr size_t MAX_TERMINAL_LINES = 1000;

// Replace the loadDirectoryQuick method with this optimized version
void CursorClone::loadDirectoryQuick(const std::string& path) {
    try {
        if (path.empty()) {
            throw std::runtime_error("Empty path provided");
        }

        std::error_code ec;
        fs::path dir_path = fs::absolute(path, ec);
        if (ec) {
            throw std::runtime_error("Failed to resolve path: " + ec.message());
        }

        if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
            throw std::runtime_error("Path does not exist or is not a directory");
        }

        // Clear existing files
        current_directory_files.clear();

        // Safely iterate directory
        for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
            if (ec) {
                std::cerr << "Warning: Error reading entry: " << ec.message() << std::endl;
                continue;
            }
            current_directory_files.push_back(entry);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error loading directory: " << e.what() << std::endl;
        current_directory_files.clear();  // Ensure vector is empty on error
    } catch (...) {
        std::cerr << "Unknown error loading directory" << std::endl;
        current_directory_files.clear();
    }
}

// Add this helper method to get the home directory
std::string CursorClone::getHomeDirectory() {
    #if defined(_WIN32)
        // Windows: Use USERPROFILE environment variable
        const char* home = std::getenv("USERPROFILE");
        if (home) return home;
        // Fallback to HOMEDRIVE + HOMEPATH
        const char* drive = std::getenv("HOMEDRIVE");
        const char* path = std::getenv("HOMEPATH");
        if (drive && path) return std::string(drive) + path;
    #else
        // Unix-like systems (Linux, macOS): Use HOME environment variable
        const char* home = std::getenv("HOME");
        if (home) return home;
    #endif
    // Fallback to current directory if home can't be determined
    return fs::current_path().string();
}

// Add a method to get the path display string
std::string CursorClone::getDisplayPath(const std::string& path) {
    // Use static cache with timeout
    static std::string last_path;
    static std::string last_result;
    static auto last_update = std::chrono::steady_clock::now();
    
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - last_update).count();
    
    // Return cached result if path hasn't changed and cache is fresh
    if (path == last_path && age < 5) {
        return last_result;
    }
    
    try {
        static const fs::path home_path = getHomeDirectory();  // Cache home path
        fs::path full_path(path);
        
        std::error_code ec;
        if (fs::relative(full_path, home_path, ec).native().find("..") == std::string::npos) {
            #if defined(_WIN32)
                last_result = "~\\" + fs::relative(full_path, home_path).string();
            #else
                last_result = "~/" + fs::relative(full_path, home_path).string();
            #endif
        } else {
            last_result = full_path.string();
        }
        
        last_path = path;
        last_update = now;
        return last_result;
        
    } catch (...) {
        return path;
    }
}

// Add a method to clear the cache when needed
void CursorClone::clearDirectoryCache() {
    directory_cache.clear();
}

// Update the forceDirectoryRefresh method to be more aggressive
void CursorClone::forceDirectoryRefresh() {
    try {
        needs_refresh = true;
        last_directory.clear();  // Clear last directory to force refresh
        last_refresh_time = std::chrono::steady_clock::now();
        
        // Clear the cache
        clearDirectoryCache();
        
        // Immediate refresh
        loadDirectoryQuick(current_directory);
        
        // Schedule multiple refreshes to catch delayed filesystem updates
        std::thread([this]() {
            for (int i = 0; i < 3; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * (i + 1)));
                needs_refresh = true;
            }
        }).detach();
        
    } catch (const std::exception& e) {
        std::cerr << "Error during directory refresh: " << e.what() << std::endl;
    }
}

// Add this method to show the browse button in the file manager section
void CursorClone::showBrowseButton() {
    if (ImGui::Button("Browse")) {
        openSystemDirectoryBrowser();  // Directly call the native file dialog
    }
}

// Add this helper method to get the current username
std::string CursorClone::getUsername() {
    #if defined(_WIN32)
        char username[UNLEN + 1];
        DWORD username_len = UNLEN + 1;
        GetUserName(username, &username_len);
        return std::string(username);
    #else
        const char* user = std::getenv("USER");
        if (!user) user = std::getenv("LOGNAME");
        if (!user) return "user";
        return std::string(user);
    #endif
}

// Add this helper method to get the hostname
std::string CursorClone::getHostname() {
    #if defined(_WIN32)
        char hostname[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD hostname_len = sizeof(hostname);
        GetComputerName(hostname, &hostname_len);
        return std::string(hostname);
    #else
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            return "localhost";
        }
        return std::string(hostname);
    #endif
}

// Add this method to send input to the running process
void CursorClone::sendInput(const std::string& input) {
    if (!async_command || !async_command->running) {
        return;
    }

    // Write the input to the pipe
    if (async_command->input_pipe[1] != -1) {
        write(async_command->input_pipe[1], input.c_str(), input.length());
    }
    async_command->waiting_for_input = false;
}

void CursorClone::executeAIGeneratedCode(const std::string& code, const std::string& filename) {
    try {
        // Add a clear visual separator
        terminal_history.push_back("\n╔════════════════════════════════════════════════════════════╗");
        terminal_history.push_back("║                   AI Generated Code                         ║");
        terminal_history.push_back("╠════════════════════════════════════════════════════════════╣");
        
        // Detect the language and set up the environment
        std::string ext = fs::path(filename).extension().string();
        std::string interpreter;
        std::string shebang;
        std::string setup_cmd;
        
        if (ext == ".py") {
            interpreter = "python3";
            shebang = "#!/usr/bin/env python3\n# -*- coding: utf-8 -*-\n\n";
            setup_cmd = "python3 -m pip install --user --quiet virtualenv && "
                       "python3 -m virtualenv .venv && "
                       "source .venv/bin/activate && "
                       "pip install --quiet -r requirements.txt 2>/dev/null || true && ";
        } else if (ext == ".js") {
            interpreter = "node";
            shebang = "#!/usr/bin/env node\n\n";
            setup_cmd = "npm install 2>/dev/null || true && ";
        } else {
            throw std::runtime_error("Unsupported file extension: " + ext);
        }

        // Create and write the script file
        fs::path script_path = fs::path(current_directory) / filename;
        std::ofstream file(script_path);
        if (!file.is_open()) {
            throw std::runtime_error("Could not create file: " + filename);
        }
        
        // Write shebang and code
        file << shebang << code;
        file.close();
        
        // Set execute permissions
        #if !defined(_WIN32)
        fs::permissions(script_path, 
            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
            fs::perm_options::add);
        #endif

        // Force directory refresh
        forceDirectoryRefresh();
        
        // Execute the script with proper setup and interpreter
        std::string command = setup_cmd + interpreter + " \"" + script_path.string() + "\"";
        executeCommandAsync(command);
        
    } catch (const std::exception& e) {
        terminal_history.push_back("Error executing AI code: " + std::string(e.what()));
    }
}

// Add this method to generate embeddings for directories
std::vector<float> CursorClone::getDirectoryEmbedding(const std::string& path) {
    if (!embeddings_enabled) return std::vector<float>();
    
    try {
        std::string directory_content;
        for (const auto& entry : fs::directory_iterator(path)) {
            std::string entry_type = entry.is_directory() ? "[DIR] " : "[FILE] ";
            directory_content += entry_type + entry.path().filename().string() + "\n";
            
            // Add file metadata if it's a file
            if (!entry.is_directory()) {
                auto file_size = entry.file_size();
                auto last_write = entry.last_write_time();
                
                // Convert to time_t in a C++17 compatible way
                auto duration = last_write.time_since_epoch();
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                time_t time_t_time = static_cast<time_t>(seconds);
                
                directory_content += "  Size: " + std::to_string(file_size) + " bytes\n";
                directory_content += "  Modified: " + std::string(std::ctime(&time_t_time));
            }
        }
        
        // Get embedding from Groq
        return groq_client.getEmbedding(directory_content);
        
    } catch (const std::exception& e) {
        std::cerr << "Error generating directory embedding: " << e.what() << std::endl;
        return std::vector<float>();
    }
}

// Add this method to update Pinecone when directory changes
void CursorClone::updateDirectoryEmbedding(const std::string& path) {
    if (!embeddings_enabled) return;
    
    try {
        std::string directory_content;
        for (const auto& entry : fs::directory_iterator(path)) {
            std::string entry_type = entry.is_directory() ? "[DIR] " : "[FILE] ";
            directory_content += entry_type + entry.path().filename().string() + "\n";
            
            // Add file metadata if it's a file
            if (!entry.is_directory()) {
                auto file_size = entry.file_size();
                auto last_write = entry.last_write_time();
                
                // Convert to time_t in a C++17 compatible way
                auto duration = last_write.time_since_epoch();
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                time_t time_t_time = static_cast<time_t>(seconds);
                
                directory_content += "  Size: " + std::to_string(file_size) + " bytes\n";
                directory_content += "  Modified: " + std::string(std::ctime(&time_t_time));
            }
        }
        
        // Generate a unique ID for the directory
        std::string id = "dir_" + std::to_string(std::hash<std::string>{}(path));
        
        // Create metadata
        nlohmann::json metadata = {
            {"path", path},
            {"last_updated", std::chrono::system_clock::now().time_since_epoch().count()},
            {"file_count", std::distance(fs::directory_iterator(path), fs::directory_iterator())},
            {"parent", fs::path(path).parent_path().string()}
        };
        
        // Update Pinecone
        pinecone_client->upsertText(id, directory_content, metadata);
        
        std::cout << "Updated embedding for directory: " << path << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error updating directory embedding: " << e.what() << std::endl;
    }
}

// Add this method to find similar directories
void CursorClone::findSimilarDirectories(const std::string& path, int top_k) {
    if (!embeddings_enabled) return;
    
    try {
        std::string directory_content;
        for (const auto& entry : fs::directory_iterator(path)) {
            std::string entry_type = entry.is_directory() ? "[DIR] " : "[FILE] ";
            directory_content += entry_type + entry.path().filename().string() + "\n";
        }
        
        pinecone_client->queryText(directory_content, top_k);
        
    } catch (const std::exception& e) {
        std::cerr << "Error finding similar directories: " << e.what() << std::endl;
    }
}

// Add this method to the CursorClone class to handle script input
void CursorClone::handleScriptInput(int input_pipe) {
    static char input_buffer[1024] = "";
    static bool input_active = false;
    
    // Only show input field when waiting for input
    if (!input_active) {
        ImGui::SetKeyboardFocusHere();
        input_active = true;
    }

    // Create an input field that matches the terminal style
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    
    if (ImGui::InputText("##ScriptInput", input_buffer, sizeof(input_buffer),
        ImGuiInputTextFlags_EnterReturnsTrue)) {
        // Format and display the input
        std::string formatted_input = "║ > " + std::string(input_buffer);
        if (formatted_input.length() < 54) {
            formatted_input += std::string(54 - formatted_input.length(), ' ');
        }
        formatted_input += "║";
        terminal_history.push_back(formatted_input);
        
        // Send input to the process
        std::string input_str = std::string(input_buffer) + "\n";
        write(input_pipe, input_str.c_str(), input_str.length());
        
        // Clear the input buffer
        input_buffer[0] = '\0';
        input_active = false;
    }
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Add new method to check if command is waiting for input
bool CursorClone::isWaitingForInput() const {
    return async_command && async_command->waiting_for_input;
} 