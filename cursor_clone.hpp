#pragma once

#include <string>
#include "groq_client.hpp"
#include "text_editor.hpp"
#include <imgui.h>
#include <filesystem>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <future>
#include <unordered_map>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <chrono>

// Add Windows-specific includes
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <lmcons.h> // For UNLEN
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

class CursorClone {
private:
    // Add this at the top of the private section
    static constexpr size_t MAX_TERMINAL_LINES = 1000;
    
    GroqClient groq_client;
    TextEditor editor;
    std::string chat_history;
    char input_buffer[4096];
    char file_path_buffer[1024];
    
    // Remove ImGuiFileDialog
    // ImGuiFileDialog* fileDialog;
    
    // Add these new members for fonts
    ImFont* defaultFont = nullptr;
    ImFont* monoFont = nullptr;
    void setupFonts();
    
    // Existing members
    std::string current_directory;
    std::vector<fs::directory_entry> current_directory_files;
    size_t max_files_per_page = 100;
    bool is_loading_directory = false;
    std::string loading_status;

    // Add these new members
    std::mutex directory_mutex;
    std::future<void> directory_loader;
    std::unordered_map<std::string, std::vector<fs::directory_entry>> directory_cache;
    static const size_t MAX_CACHE_SIZE = 10;
    bool cancel_loading = false;

    // Terminal related members
    struct TerminalCommand {
        std::string output;
        bool running = false;
    };
    
    std::deque<std::string> terminal_history;
    std::vector<std::string> command_history;
    size_t command_history_index = 0;
    std::string terminal_input;
    char terminal_buffer[1024] = {0};
    std::unique_ptr<TerminalCommand> current_command;
    void executeTerminalCommand(const std::string& command);
    void updateTerminalOutput();
    static constexpr size_t MAX_HISTORY = 1000;
    static constexpr size_t MAX_COMMAND_HISTORY = 100;
    
    // Terminal navigation
    bool handleTerminalInput(ImGuiInputTextCallbackData* data);
    static int terminalInputCallback(ImGuiInputTextCallbackData* data);
    void navigateCommandHistory(bool up);

    std::string readFile(const std::string& path);
    void writeFile(const std::string& path, const std::string& content);
    void refreshDirectoryContent();
    std::string getFileIcon(const fs::directory_entry& entry);
    void navigateToParentDirectory();
    void changeDirectory(const std::string& new_path);
    void showDirectoryContextMenu(const fs::path& path);
    void openSystemDirectoryBrowser();
    bool isRecognizedFileType(const std::string& ext);

    // Add these new methods
    void loadDirectoryAsync(const std::string& path);
    void clearOldCache();
    void cancelCurrentLoading();
    void clearDirectoryCache();

    std::string getHomeDirectory();
    std::string getDisplayPath(const std::string& path);

    void loadDirectoryQuick(const std::string& path);

    // Directory refresh state
    static bool needs_refresh;
    static std::string last_directory;
    static std::chrono::steady_clock::time_point last_refresh_time;

    void forceDirectoryRefresh();

    // Add to private section of CursorClone class:
    struct AsyncCommand {
        std::future<void> future;
        FILE* pipe;
        bool running;
        bool command_running;
        std::string command;
        std::deque<std::string> output_buffer;
        std::mutex output_mutex;
        int master_fd;
        bool waiting_for_input;
        
        AsyncCommand() : pipe(nullptr), 
                        running(false), 
                        command_running(false),
                        master_fd(-1), 
                        waiting_for_input(false) {}
    };

    // Add the sendInput method declaration
    void sendInput(const std::string& input);

    std::unique_ptr<AsyncCommand> async_command;
    void executeCommandAsync(const std::string& command);
    void checkCommandOutput();

    bool isTextFile(const std::string& ext) {
        static const std::unordered_set<std::string> text_extensions = {
            ".txt", ".py", ".cpp", ".h", ".hpp", ".c", ".js", ".ts", ".html", ".css",
            ".json", ".xml", ".yaml", ".yml", ".md", ".rst", ".ini", ".conf",
            ".sh", ".bash", ".pl", ".rb", ".php", ".java", ".go", ".rs"
        };
        return text_extensions.find(ext) != text_extensions.end();
    }

    void showBrowseButton();

    std::string getUsername();
    std::string getHostname();

public:
    CursorClone(const std::string& api_key);
    ~CursorClone();
    void renderGUI();
    std::string getAIAssistance(const std::string& query);
}; 