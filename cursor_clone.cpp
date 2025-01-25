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
    input_buffer[0] = '\0';
    file_path_buffer[0] = '\0';
    
    try {
        // Start in home directory
        current_directory = getHomeDirectory();
        std::cout << "Starting in home directory: " << current_directory << std::endl;
        
        refreshDirectoryContent();
        setupFonts();
        
    } catch (const std::exception& e) {
        std::cerr << "Error initializing: " << e.what() << std::endl;
        current_directory = fs::current_path().string();
        refreshDirectoryContent();
    }
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
        
    } catch (const std::exception& e) {
        std::cerr << "Error writing file " << path << ": " << e.what() << std::endl;
        throw;
    }
}

std::string CursorClone::getAIAssistance(const std::string& query) {
    try {
        std::string currentFileName = editor.getCurrentFile();
        std::string fileContent = editor.getContent();
        size_t cursorPos = editor.getCursorPosition();
        
        // Build context-aware prompt
        std::string prompt = "You are an AI programming assistant. ";
        prompt += "\nCurrent working directory: " + current_directory + "\n";
        
        // Add directory contents information
        prompt += "\nFiles in current directory:\n";
        for (const auto& entry : current_directory_files) {
            std::string name = entry.path().filename().string();
            if (entry.is_directory()) {
                prompt += "[DIR] " + name + "/\n";
            } else {
                prompt += "[FILE] " + name + "\n";
                // If it's a recognized text file, add its content
                std::string ext = entry.path().extension().string();
                if (isRecognizedFileType(ext)) {
                    try {
                        std::string content = readFile(entry.path().string());
                        if (content.length() < 10000) {  // Only include smaller files
                            prompt += "Content of " + name + ":\n```\n" + content + "\n```\n";
                        } else {
                            prompt += "(File too large to include content)\n";
                        }
                    } catch (...) {
                        // Ignore files we can't read
                    }
                }
            }
        }

        if (!currentFileName.empty()) {
            // Determine if we're working with a Python file
            bool isPythonFile = false;
            std::string fileExtension;
            size_t dot_pos = currentFileName.find_last_of('.');
            if (dot_pos != std::string::npos) {
                fileExtension = currentFileName.substr(dot_pos);
                isPythonFile = (fileExtension == ".py");
            }

            // Build context-aware prompt
            prompt += "You can modify the file content directly. ";
            
            if (isPythonFile) {
                prompt += "\nThis is a Python file. Please ensure suggestions follow PEP 8 style guidelines ";
                prompt += "and Python best practices. Consider proper indentation and Python-specific patterns.\n";
            }
            
            // Add file context
            prompt += "\nCurrent file: " + currentFileName;
            prompt += " (." + fileExtension + " file)";
            
            // Add the file content with cursor position marker
            std::string contentWithCursor = std::string(fileContent);
            contentWithCursor.insert(cursorPos, "<!CURSOR!>");
            prompt += "\n\nFile content (<!CURSOR!> marks cursor position):\n```";
            prompt += isPythonFile ? "python" : ""; // Specify language for better formatting
            prompt += "\n" + contentWithCursor + "\n```\n\n";
            
            prompt += "Cursor position: " + std::to_string(cursorPos) + "\n\n";

            // Add edit instructions
            prompt += "\nYou can modify the file using these commands:\n";
            prompt += "1. To insert at cursor: <INSERT>new code</INSERT>\n";
            prompt += "2. To replace text: <REPLACE start=X end=Y>new code</REPLACE>\n";
            prompt += "3. To delete text: <DELETE start=X end=Y>\n";
            prompt += "4. To create new files: <NEW_FILE path=\"filename\">content</NEW_FILE>\n\n";
            
        } else {
            // Add special handling for no file open
            prompt += "\nNo file is currently open. I can help you:\n";
            prompt += "1. Create a new file in the current directory using: <NEW_FILE path=\"filename.ext\">content</NEW_FILE>\n";
            prompt += "2. Create a new project in the current directory\n";
            prompt += "3. Set up project structure and build files\n\n";
            
            // Add project templates hint if query suggests creating a project
            if (query.find("create project") != std::string::npos || 
                query.find("new project") != std::string::npos ||
                query.find("setup project") != std::string::npos) {
                    
                prompt += "Available project templates:\n";
                prompt += "- C++ project (CMake based)\n";
                prompt += "- Python project (with virtual environment)\n";
                prompt += "- Web project (HTML/CSS/JS)\n";
                prompt += "\nI'll create all necessary files in the current directory: " + current_directory + "\n";
            }
        }

        // Add terminal command instructions to the prompt
        prompt += "\nYou can execute terminal commands using: <TERMINAL>command</TERMINAL>\n";
        prompt += "For example:\n";
        prompt += "- <TERMINAL>python3 script.py</TERMINAL>\n";
        prompt += "- <TERMINAL>g++ file.cpp -o program && ./program</TERMINAL>\n";
        prompt += "- <TERMINAL>ls</TERMINAL>\n\n";

        // Add write file instructions to the prompt
        prompt += "\nYou can write to any file using: <WRITE_FILE path=\"filename\">content</WRITE_FILE>\n";
        prompt += "This will write the content to the file even if it's not currently open.\n";

        // Get the AI response
        std::string response = groq_client.getCompletion(prompt + "\n\nUser: " + query);
        bool made_changes = false;

        // Handle terminal commands
        size_t terminal_pos = 0;
        while ((terminal_pos = response.find("<TERMINAL>", terminal_pos)) != std::string::npos) {
            size_t end_pos = response.find("</TERMINAL>", terminal_pos);
            if (end_pos != std::string::npos) {
                std::string command = response.substr(terminal_pos + 10, end_pos - (terminal_pos + 10));
                
                // Add command to terminal history
                terminal_history.push_back("> " + command);
                
                // Execute the command
                executeTerminalCommand(command);
                made_changes = true;
            }
            terminal_pos = end_pos;
        }

        // Handle file modifications if a file is open
        if (editor.isFileOpen()) {
            // Handle INSERT tags
            size_t insert_pos = 0;
            while ((insert_pos = response.find("<INSERT>", insert_pos)) != std::string::npos) {
                size_t end_pos = response.find("</INSERT>", insert_pos);
                if (end_pos != std::string::npos) {
                    std::string code = response.substr(insert_pos + 8, end_pos - (insert_pos + 8));
                    editor.insertAtCursor(code);
                    made_changes = true;
                }
                insert_pos = end_pos;
            }

            // Handle REPLACE tags
            size_t replace_pos = 0;
            while ((replace_pos = response.find("<REPLACE", replace_pos)) != std::string::npos) {
                size_t start_attr = response.find("start=", replace_pos) + 6;
                size_t end_attr = response.find("end=", replace_pos) + 4;
                size_t close_tag = response.find(">", replace_pos);
                size_t end_tag = response.find("</REPLACE>", close_tag);
                
                if (start_attr != std::string::npos && end_attr != std::string::npos && 
                    close_tag != std::string::npos && end_tag != std::string::npos) {
                    
                    size_t start_pos = std::stoul(response.substr(start_attr, response.find(" ", start_attr) - start_attr));
                    size_t end_pos = std::stoul(response.substr(end_attr, response.find(">", end_attr) - end_attr));
                    std::string new_code = response.substr(close_tag + 1, end_tag - (close_tag + 1));
                    
                    editor.replaceText(start_pos, end_pos, new_code);
                    made_changes = true;
                }
                replace_pos = end_tag;
            }

            // Handle DELETE tags
            size_t delete_pos = 0;
            while ((delete_pos = response.find("<DELETE", delete_pos)) != std::string::npos) {
                size_t start_attr = response.find("start=", delete_pos) + 6;
                size_t end_attr = response.find("end=", delete_pos) + 4;
                size_t end_tag = response.find(">", delete_pos);
                
                if (start_attr != std::string::npos && end_attr != std::string::npos) {
                    size_t start_pos = std::stoul(response.substr(start_attr, response.find(" ", start_attr) - start_attr));
                    size_t end_pos = std::stoul(response.substr(end_attr, response.find(">", end_attr) - end_attr));
                    
                    editor.replaceText(start_pos, end_pos, "");
                    made_changes = true;
                }
                delete_pos = end_tag;
            }

            // Save changes if any modifications were made
            if (made_changes) {
                editor.saveFile();
                response += "\n\nChanges have been applied and saved to the file.";
            }
        }

        // Handle NEW_FILE tags
        size_t file_pos = 0;
        while ((file_pos = response.find("<NEW_FILE", file_pos)) != std::string::npos) {
            size_t path_start = response.find("path=\"", file_pos) + 6;
            size_t path_end = response.find("\"", path_start);
            size_t close_tag = response.find(">", file_pos);
            size_t end_tag = response.find("</NEW_FILE>", close_tag);
            
            if (path_start != std::string::npos && path_end != std::string::npos && 
                close_tag != std::string::npos && end_tag != std::string::npos) {
                
                std::string filepath = response.substr(path_start, path_end - path_start);
                std::string content = response.substr(close_tag + 1, end_tag - (close_tag + 1));
                
                try {
                    // Create the file in the current directory
                    fs::path file_path = fs::path(current_directory) / fs::path(filepath).filename();
                    
                    // Create parent directories if needed
                    if (file_path.has_parent_path()) {
                        fs::create_directories(file_path.parent_path());
                    }
                    
                    // Write the file
                    writeFile(file_path.string(), content);
                    std::cout << "Created new file: " << file_path.string() << std::endl;
                    made_changes = true;
                    
                    // If no file is currently open, open the new file
                    if (!editor.isFileOpen()) {
                        editor.openFile(file_path.string());
                    }
                    
                    // Force immediate directory refresh
                    forceDirectoryRefresh();
                    
                } catch (const std::exception& e) {
                    std::cerr << "Error creating file " << filepath << ": " << e.what() << std::endl;
                }
            }
            file_pos = end_tag;
        }

        // Handle WRITE_FILE tags
        size_t write_pos = 0;
        while ((write_pos = response.find("<WRITE_FILE", write_pos)) != std::string::npos) {
            size_t path_start = response.find("path=\"", write_pos) + 6;
            size_t path_end = response.find("\"", path_start);
            size_t close_tag = response.find(">", write_pos);
            size_t end_tag = response.find("</WRITE_FILE>", close_tag);
            
            if (path_start != std::string::npos && path_end != std::string::npos && 
                close_tag != std::string::npos && end_tag != std::string::npos) {
                
                std::string filepath = response.substr(path_start, path_end - path_start);
                std::string content = response.substr(close_tag + 1, end_tag - (close_tag + 1));
                
                try {
                    // Create the file in the current directory
                    fs::path file_path = fs::path(current_directory) / fs::path(filepath).filename();
                    
                    // Create parent directories if needed
                    if (file_path.has_parent_path()) {
                        fs::create_directories(file_path.parent_path());
                    }
                    
                    // Write the file
                    writeFile(file_path.string(), content);
                    std::cout << "Written to file: " << file_path.string() << std::endl;
                    made_changes = true;
                    
                    // Force immediate directory refresh
                    forceDirectoryRefresh();
                    
                } catch (const std::exception& e) {
                    std::cerr << "Error writing to file " << filepath << ": " << e.what() << std::endl;
                }
            }
            write_pos = end_tag;
        }

        if (made_changes) {
            response += "\n\nFiles have been written and directory has been updated.";
        }
        
        return response;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception in getAIAssistance: " << e.what() << std::endl;
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

    // Static variables for state tracking
    static bool needs_refresh = false;
    static std::string last_directory;
    static auto last_refresh_time = std::chrono::steady_clock::now();

    // Only refresh directory when needed and not too frequently
    auto refresh_check_time = std::chrono::steady_clock::now();
    auto time_since_refresh = std::chrono::duration_cast<std::chrono::milliseconds>(refresh_check_time - last_refresh_time);

    if (current_directory != last_directory && time_since_refresh.count() > 500) {
        needs_refresh = true;
        last_directory = current_directory;
        last_refresh_time = refresh_check_time;
    }

    if (needs_refresh) {
        loadDirectoryQuick(current_directory);
        needs_refresh = false;
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
    ImGui::BeginChild("Terminal", ImVec2(0, 0), true);
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
            
            // Make the text selectable
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.3f, 0.3f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));

            // Use Selectable with text wrapping
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
            ImGui::Selectable(line.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
            ImGui::PopTextWrapPos();

            // Handle text selection
            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseClicked(0)) {
                    ImGui::SetKeyboardFocusHere();
                }
                
                // Handle copy when Ctrl+C is pressed
                if (ImGui::IsKeyPressed(ImGuiKey_C) && ImGui::GetIO().KeyCtrl) {
                    ImGui::SetClipboardText(line.c_str());
                }
            }

            ImGui::PopStyleColor(4);  // Pop all colors
        }

        // Current input line
        if (!async_command || !async_command->command_running) {  // Only show command prompt when no command is running
            // Show the full command prompt
            std::string prompt = getUsername() + "@" + getHostname() + ":" + getDisplayPath(current_directory) + "$ ";
            ImGui::PushStyleColor(ImGuiCol_Text, TerminalColors::Prompt);
            ImGui::TextUnformatted(prompt.c_str());
            ImGui::PopStyleColor();
        } else if (async_command && async_command->waiting_for_input) {  // For input prompts, just show the input field
            ImGui::PushStyleColor(ImGuiCol_Text, TerminalColors::Default);
            ImGui::TextUnformatted("> ");  // Simple input indicator
            ImGui::PopStyleColor();
        }

        // Show input field for both command input and program input
        if ((!async_command || !async_command->command_running) || 
            (async_command && async_command->waiting_for_input)) {
            ImGui::SameLine();
            
            // Calculate remaining width for input
            float remaining_width = ImGui::GetContentRegionAvail().x;
            
            // Input text that appears to be part of the terminal
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            
            ImGui::SetNextItemWidth(remaining_width);
            bool input_received = ImGui::InputText("##TerminalInput", 
                terminal_buffer, 
                sizeof(terminal_buffer),
                ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_CallbackHistory |
                ImGuiInputTextFlags_CallbackCompletion,
                terminalInputCallback,
                this
            );

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();

            // Handle input
            if (input_received) {
                std::string command = terminal_buffer;
                if (!command.empty()) {
                    if (async_command && async_command->waiting_for_input) {
                        sendInput(command);
                    } else if (!async_command || !async_command->command_running) {
                        executeTerminalCommand(command);
                    }
                    terminal_buffer[0] = '\0';
                }
                ImGui::SetKeyboardFocusHere(-1);
            }
        }

        // Auto-scroll
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
    ImGui::TextWrapped("%s", chat_history.c_str());
    
    // Auto-scroll to bottom when new content is added
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // Input area with improved handling
    ImGui::PushItemWidth(-1); // Make input field fill the width
    bool input_submitted = ImGui::InputText("##AIInput", input_buffer, sizeof(input_buffer), 
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    if (ImGui::Button("Send") || input_submitted) {
        if (strlen(input_buffer) > 0) {
            std::string query = input_buffer;
            chat_history += "\nYou: " + query + "\n";
            
            // Get AI response
            std::string response = getAIAssistance(query);
            chat_history += "Assistant: " + response + "\n";
            
            // Clear input buffer
            input_buffer[0] = '\0';
            
            // Force scroll to bottom
            ImGui::SetScrollHereY(1.0f);
        }
    }
    
    ImGui::EndChild();
    ImGui::PopFont();
    
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
        
        if (!parent.empty()) {
            changeDirectory(parent.string());
        }
    } catch (const std::exception& e) {
        std::cerr << "Error navigating to parent: " << e.what() << std::endl;
    }
}

void CursorClone::changeDirectory(const std::string& new_path) {
    try {
        std::error_code ec;
        fs::path target_path = fs::absolute(new_path, ec);
        
        if (!ec && fs::is_directory(target_path, ec)) {
            current_directory = target_path.string();
            loadDirectoryQuick(current_directory);
            
            // Force immediate directory refresh
            needs_refresh = true;
            last_directory = current_directory;
            last_refresh_time = std::chrono::steady_clock::now();
            refreshDirectoryContent();
            
            // Update terminal history with new path
            terminal_history.push_back("Changed directory to: " + getDisplayPath(current_directory));
        } else {
            terminal_history.push_back("cd: " + new_path + ": No such directory");
        }
    } catch (const std::exception& e) {
        terminal_history.push_back("Error changing directory: " + std::string(e.what()));
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
                    refreshDirectoryContent();
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
                refreshDirectoryContent();
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
    #if defined(_WIN32)
        // Windows implementation
        BROWSEINFO bi = { 0 };
        bi.lpszTitle = "Select Directory";
        LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
        if (pidl != 0) {
            char path[MAX_PATH];
            if (SHGetPathFromIDList(pidl, path)) {
                changeDirectory(path);
            }
            IMalloc* imalloc = 0;
            if (SUCCEEDED(SHGetMalloc(&imalloc))) {
                imalloc->Free(pidl);
                imalloc->Release();
            }
        }
    #elif defined(__APPLE__)
        // macOS implementation
        std::string command = "osascript -e 'choose folder with prompt \"Select Directory\"'";
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe) {
            char buffer[1024];
            std::string result = "";
            while (!feof(pipe)) {
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    result += buffer;
                }
            }
            pclose(pipe);
            if (!result.empty()) {
                // Remove newline at the end
                result = result.substr(0, result.length() - 1);
                changeDirectory(result);
            }
        }
    #else
        // Linux implementation using zenity
        std::string command = "zenity --file-selection --directory";
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe) {
            char buffer[1024];
            std::string result = "";
            while (!feof(pipe)) {
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    result += buffer;
                }
            }
            pclose(pipe);
            if (!result.empty()) {
                // Remove newline at the end
                result = result.substr(0, result.length() - 1);
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

        if (command.substr(0, 3) == "cd ") {
            std::string path = command.substr(3);
            // Trim whitespace
            path.erase(0, path.find_first_not_of(" \t"));
            path.erase(path.find_last_not_of(" \t") + 1);
            
            if (path.empty() || path == "~") {
                changeDirectory(getHomeDirectory());
            } else {
                changeDirectory(path);
            }
            return;
        }

        // Add the command to history with the full prompt
        std::string prompt = getUsername() + "@" + getHostname() + ":" + getDisplayPath(current_directory) + "$ ";
        terminal_history.push_back(prompt + command);

        // Execute the command in the current directory
        executeCommandAsync(command);

    } catch (const std::exception& e) {
        terminal_history.push_back("Error: " + std::string(e.what()));
    }
}

void CursorClone::executeCommandAsync(const std::string& command) {
    try {
        // Cancel any existing command
        if (async_command && async_command->running) {
            #ifdef _WIN32
                _pclose(async_command->pipe);
            #else
                pclose(async_command->pipe);
            #endif
            async_command->pipe = nullptr;
            async_command->running = false;
        }

        async_command = std::make_unique<AsyncCommand>();
        async_command->command = command;
        async_command->running = true;
        async_command->command_running = true;  // Set this when starting command

        async_command->future = std::async(std::launch::async, [this, command]() {
            try {
                char buffer[4096];  // Use 4KB buffer consistently
                
                #ifdef _WIN32
                    // Windows code...
                #else
                    // For Unix, handle Python specially
                    std::string full_command;
                    if (command.find("python") != std::string::npos) {
                        // Extract Python command and arguments
                        size_t space_pos = command.find(" ");
                        std::string python_cmd = space_pos != std::string::npos ? 
                            command.substr(0, space_pos) : command;
                        std::string args = space_pos != std::string::npos ? 
                            command.substr(space_pos + 1) : "";
                            
                        // Trim whitespace from args
                        args.erase(0, args.find_first_not_of(" \t"));
                        
                        // If it's a .py file, use full path
                        if (args.find(".py") != std::string::npos) {
                            fs::path script_path = fs::path(current_directory) / args;
                            full_command = "PYTHONUNBUFFERED=1 PYTHONIOENCODING=utf-8 " + 
                                         python_cmd + " -u \"" + script_path.string() + "\"";
                        } else {
                            full_command = "PYTHONUNBUFFERED=1 PYTHONIOENCODING=utf-8 " + 
                                         python_cmd + " -u " + args;
                        }
                    } else {
                        full_command = command;
                    }

                    // Create PTY with proper terminal settings
                    int master, slave;
                    char name[1024];
                    
                    if (openpty(&master, &slave, name, nullptr, nullptr) == -1) {
                        throw std::runtime_error("Failed to open PTY");
                    }

                    // Set up terminal attributes for proper output handling
                    struct termios tios;
                    tcgetattr(slave, &tios);
                    cfmakeraw(&tios);  // Use raw mode
                    tios.c_lflag &= ~(ECHO | ICANON);  // Disable echo and canonical mode
                    tios.c_cc[VMIN] = 1;   // Read one char at a time
                    tios.c_cc[VTIME] = 0;  // No timeout
                    tcsetattr(slave, TCSANOW, &tios);

                    pid_t pid = fork();
                    if (pid == -1) {
                        close(master);
                        close(slave);
                        throw std::runtime_error("Fork failed");
                    }
                    
                    if (pid == 0) {  // Child process
                        close(master);
                        setsid();
                        dup2(slave, 0);  // stdin
                        dup2(slave, 1);  // stdout
                        dup2(slave, 2);  // stderr
                        close(slave);
                        
                        if (chdir(current_directory.c_str()) != 0) {
                            std::cerr << "Failed to change directory to: " << current_directory << std::endl;
                            exit(1);
                        }

                        execl("/bin/bash", "bash", "-c", full_command.c_str(), nullptr);
                        perror("execl failed");
                        exit(1);
                    }
                    
                    // Parent process
                    close(slave);
                    async_command->master_fd = master;
                    
                    // Set non-blocking mode
                    int flags = fcntl(master, F_GETFL, 0);
                    fcntl(master, F_SETFL, flags | O_NONBLOCK);
                    
                    std::string partial_line;
                    
                    while (async_command->running) {
                        fd_set readfds;
                        FD_ZERO(&readfds);
                        FD_SET(master, &readfds);
                        
                        struct timeval tv;
                        tv.tv_sec = 0;
                        tv.tv_usec = 1000;  // 1ms timeout for more responsive output
                        
                        int ret = select(master + 1, &readfds, nullptr, nullptr, &tv);
                        if (ret > 0) {
                            ssize_t bytes_read = read(master, buffer, sizeof(buffer) - 1);
                            if (bytes_read <= 0) {
                                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                    break;
                                }
                                continue;
                            }
                            
                            buffer[bytes_read] = '\0';
                            
                            // Immediately add output to terminal history
                            std::lock_guard<std::mutex> lock(async_command->output_mutex);
                            std::string output(buffer, bytes_read);
                            
                            // Split output into lines
                            size_t start = 0;
                            size_t end;
                            
                            while ((end = output.find('\n', start)) != std::string::npos) {
                                std::string line = partial_line + output.substr(start, end - start);
                                if (!line.empty()) {
                                    terminal_history.push_back(line);
                                    
                                    // Check for input prompts
                                    if (line.find("input(") != std::string::npos || 
                                        line.find("Input") != std::string::npos ||
                                        line.find("Enter") != std::string::npos ||
                                        line.find(": ") != std::string::npos ||
                                        line.find("?") != std::string::npos) {
                                        async_command->waiting_for_input = true;
                                    }
                                }
                                start = end + 1;
                                partial_line.clear();
                            }
                            
                            // Save any remaining partial line
                            if (start < output.length()) {
                                partial_line = output.substr(start);
                                if (!partial_line.empty()) {
                                    terminal_history.push_back(partial_line);
                                    
                                    // Check if partial line is an input prompt
                                    if (partial_line.find("input(") != std::string::npos || 
                                        partial_line.find("Input") != std::string::npos ||
                                        partial_line.find("Enter") != std::string::npos ||
                                        partial_line.find(": ") != std::string::npos ||
                                        partial_line.find("?") != std::string::npos) {
                                        async_command->waiting_for_input = true;
                                    }
                                    partial_line.clear();
                                }
                            }
                        }
                        
                        // Small sleep to prevent CPU hogging
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    
                    close(master);
                #endif

                // Reset command state only after normal completion
                async_command->running = false;
                async_command->command_running = false;
                async_command->waiting_for_input = false;

            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(async_command->output_mutex);
                async_command->output_buffer.push_back("Error: " + std::string(e.what()));
                async_command->running = false;
                async_command->command_running = false;
                async_command->waiting_for_input = false;
            }
        });

    } catch (const std::exception& e) {
        terminal_history.push_back("Error launching command: " + std::string(e.what()));
        if (async_command) {
            async_command->running = false;
            async_command->command_running = false;
            async_command->waiting_for_input = false;
        }
    }
}

void CursorClone::checkCommandOutput() {
    if (!async_command) return;

    // Check if command has finished
    if (async_command->future.valid() && 
        async_command->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        
        // Transfer any remaining output
        {
            std::lock_guard<std::mutex> lock(async_command->output_mutex);
            while (!async_command->output_buffer.empty()) {
                std::string line = async_command->output_buffer.front();
                
                // Check if this is an error message
                if (line.find("Error:") != std::string::npos || 
                    line.find("exited with code") != std::string::npos) {
                    // Reset terminal state on error
                    async_command->waiting_for_input = false;
                    async_command->command_running = false;
                }
                
                // Process the line
                if (!line.empty()) {
                    // Handle carriage return for interactive output
                    if (line[0] == '\r') {
                        if (!terminal_history.empty()) {
                            terminal_history.pop_back();
                        }
                        line = line.substr(1);
                    }
                    
                    // Remove trailing whitespace and newlines
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
                        line.pop_back();
                    }
                    
                    if (!line.empty()) {
                        terminal_history.push_back(line);
                    }
                }
                
                async_command->output_buffer.pop_front();
            }
        }
        
        // Reset command state if not waiting for input
        if (!async_command->waiting_for_input) {
            async_command->running = false;
            async_command->command_running = false;
        }
    }
}

static constexpr size_t MAX_TERMINAL_LINES = 1000;

// Replace the loadDirectoryQuick method with this optimized version
void CursorClone::loadDirectoryQuick(const std::string& path) {
    try {
        // Clear existing entries
        current_directory_files.clear();
        
        #ifdef _WIN32
            // On Windows, if we're in root, show available drives
            if (path == "C:\\" || path == "/") {
                DWORD drives = GetLogicalDrives();
                for (char drive = 'A'; drive <= 'Z'; drive++) {
                    if (drives & (1 << (drive - 'A'))) {
                        std::string drive_path = std::string(1, drive) + ":\\";
                        current_directory_files.push_back(fs::directory_entry(drive_path));
                    }
                }
                return;
            }
        #endif

        // Use static cache to persist between calls
        static std::unordered_map<std::string, std::pair<std::vector<fs::directory_entry>, std::chrono::steady_clock::time_point>> cache;
        auto now = std::chrono::steady_clock::now();
        
        // Check cache first (with 5 second timeout)
        auto it = cache.find(path);
        if (it != cache.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.second).count();
            if (age < 5) {  // Use cache if less than 5 seconds old
                current_directory_files = it->second.first;
                return;
            }
        }

        // Pre-allocate with smaller size
        std::vector<fs::directory_entry> entries;
        entries.reserve(50);

        std::error_code ec;
        
        // Add parent directory entry if not at root
        #ifdef _WIN32
            if (path.length() > 3)  // More than just drive letter (e.g., "C:\")
        #else
            if (path != "/")
        #endif
        {
            entries.push_back(fs::directory_entry(fs::path(path).parent_path()));
        }

        // Use array instead of unordered_set for faster lookup
        static const std::array<std::string_view, 7> extensions = {
            ".cpp", ".hpp", ".h", ".py", ".txt", ".json", ".md"
        };

        // Single pass through directory with minimal allocations
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            if (ec) continue;
            
            bool should_add = false;
            if (entry.is_directory(ec)) {
                should_add = true;
            } else {
                // Fast extension check without string allocation
                std::string_view ext(entry.path().extension().c_str());
                for (const auto& valid_ext : extensions) {
                    if (ext == valid_ext) {
                        should_add = true;
                        break;
                    }
                }
            }
            
            if (should_add) {
                entries.push_back(entry);
            }
        }

        // Use stable_partition for better performance with directories
        auto partition_point = std::stable_partition(entries.begin(), entries.end(),
            [](const auto& e) { return e.is_directory(); });

        // Sort directories and files separately
        std::sort(entries.begin(), partition_point,
            [](const auto& a, const auto& b) {
                return a.path().filename() < b.path().filename();
            });
        std::sort(partition_point, entries.end(),
            [](const auto& a, const auto& b) {
                return a.path().filename() < b.path().filename();
            });

        // Update cache with timestamp
        if (cache.size() > 5) {
            cache.clear();
        }
        cache[path] = {entries, now};
        
        current_directory_files = std::move(entries);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading directory: " << e.what() << std::endl;
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

// Add this method to CursorClone class
void CursorClone::forceDirectoryRefresh() {
    needs_refresh = true;
    last_directory = current_directory;
    last_refresh_time = std::chrono::steady_clock::now();
    refreshDirectoryContent();
    clearDirectoryCache();  // Clear the cache to force a fresh read
}

// Add this method to show the browse button in the file manager section
void CursorClone::showBrowseButton() {
    if (ImGui::Button("Browse")) {
        ImGui::OpenPopup("BrowsePopup");
    }

    if (ImGui::BeginPopup("BrowsePopup")) {
        // Root directory option
        if (ImGui::MenuItem("Root Directory")) {
            #ifdef _WIN32
                changeDirectory("C:\\");
            #else
                changeDirectory("/");
            #endif
        }
        
        // Home directory option
        if (ImGui::MenuItem("Home Directory")) {
            changeDirectory(getHomeDirectory());
        }
        
        // Desktop directory option
        if (ImGui::MenuItem("Desktop")) {
            std::string desktop = getHomeDirectory();
            #ifdef _WIN32
                desktop += "\\Desktop";
            #else
                desktop += "/Desktop";
            #endif
            if (fs::exists(desktop)) {
                changeDirectory(desktop);
            }
        }
        
        // Documents directory option
        if (ImGui::MenuItem("Documents")) {
            std::string docs = getHomeDirectory();
            #ifdef _WIN32
                docs += "\\Documents";
            #else
                docs += "/Documents";
            #endif
            if (fs::exists(docs)) {
                changeDirectory(docs);
            }
        }
        
        // Downloads directory option
        if (ImGui::MenuItem("Downloads")) {
            std::string downloads = getHomeDirectory();
            #ifdef _WIN32
                downloads += "\\Downloads";
            #else
                downloads += "/Downloads";
            #endif
            if (fs::exists(downloads)) {
                changeDirectory(downloads);
            }
        }

        ImGui::EndPopup();
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

    try {
        std::string input_with_newline = input + "\n";
        
        #ifdef _WIN32
            if (async_command->pipe) {
                fputs(input_with_newline.c_str(), async_command->pipe);
                fflush(async_command->pipe);
            }
        #else
            if (async_command->master_fd >= 0) {
                write(async_command->master_fd, input_with_newline.c_str(), input_with_newline.length());
            }
        #endif
        
        // Add the input to terminal history
        terminal_history.push_back(input);
        
        // Reset input state but keep command running
        async_command->waiting_for_input = false;
        async_command->command_running = true;
        
    } catch (const std::exception& e) {
        terminal_history.push_back("Error sending input: " + std::string(e.what()));
    }
} 