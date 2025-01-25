#pragma once
#include <string>
#include <vector>
#include <memory>

class AutoComplete {
public:
    // Represents a completion suggestion
    struct Suggestion {
        std::string text;
        std::string details;
        std::string kind;
    };

    // Represents a file tab
    struct Tab {
        std::string filepath;
        std::string content;
        bool is_modified;
        size_t cursor_position;
        
        Tab(const std::string& path) : 
            filepath(path), 
            is_modified(false), 
            cursor_position(0) {}
    };

    // Represents the file management system
    class FileManager {
    public:
        std::vector<std::shared_ptr<Tab>> tabs;
        size_t active_tab_index;

        FileManager() : active_tab_index(0) {}

        // File operations
        bool openFile(const std::string& filepath);
        bool saveFile(size_t tab_index);
        bool closeTab(size_t tab_index);
        void switchTab(size_t tab_index);
        void newFile();
        
        // Tab management
        std::shared_ptr<Tab> getActiveTab() const {
            return tabs.empty() ? nullptr : tabs[active_tab_index];
        }

        // Add new methods for file tree
        std::vector<std::string> getFileTree(const std::string& root_path);
        bool isDirectory(const std::string& path);
        std::vector<std::string> listDirectory(const std::string& path);
        
        // Method to get file icon/type
        std::string getFileIcon(const std::string& filepath) {
            // Return appropriate icon based on file extension
            // Example: "📄" for files, "📁" for folders
            return isDirectory(filepath) ? "📁" : "📄";
        }
    };

    static std::vector<Suggestion> getSuggestions(
        const std::string& partial_word,
        const std::string& file_extension,
        const std::vector<std::string>& context
    ) {
        std::vector<Suggestion> suggestions;
        // Basic implementation - can be expanded later with more sophisticated completion logic
        return suggestions;
    }

    // Add method to refresh file tree
    void refreshFileTree(const std::string& root_path) {
        if (file_manager) {
            file_manager->getFileTree(root_path);
        }
    }

private:
    std::unique_ptr<FileManager> file_manager;
}; 