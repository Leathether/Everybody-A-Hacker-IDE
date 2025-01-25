// Include necessary headers
#include "cursor_clone.hpp"

void CursorClone::loadDirectoryQuick(const std::string& path) {
    // Move directory loading code here
}

void CursorClone::refreshDirectoryContent() {
    // Move directory refresh code here
}

void CursorClone::changeDirectory(const std::string& new_path) {
    try {
        std::error_code ec;
        fs::path target_path = fs::absolute(new_path, ec);
        
        if (!ec && fs::is_directory(target_path, ec)) {
            // Update the stored path
            current_directory = target_path.string();
            
            // Actually change the working directory
            if (chdir(current_directory.c_str()) != 0) {
                terminal_history.push_back("Error: Could not change to directory: " + current_directory);
                return;
            }
            
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

void CursorClone::navigateToParentDirectory() {
    // Move parent directory navigation code here
}

void CursorClone::showDirectoryContextMenu(const fs::path& path) {
    // Move context menu code here
} 