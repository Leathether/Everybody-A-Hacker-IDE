#pragma once

#include <string>
#include <vector>
#include <imgui.h>
#include <cstring>
#include <iostream>

class TextEditor {
public:
    static const size_t BUFFER_SIZE = 1024 * 1024; // 1MB buffer
    
    TextEditor() : is_file_open(false) {
        buffer = new char[BUFFER_SIZE];
        buffer[0] = '\0';
    }

    ~TextEditor() {
        delete[] buffer;
    }

    void openFile(const std::string& path);
    void saveFile();
    bool render(const ImVec2& size);
    const char* getContent() const { return buffer; }
    bool isFileOpen() const { return is_file_open; }
    const std::string& getCurrentFile() const { return current_file; }
    size_t getCursorPosition() const { return cursor_position; }
    void setCursorPosition(size_t pos) { cursor_position = pos; }
    void setContent(const std::string& content);
    void insertAtCursor(const std::string& text);
    void replaceText(size_t start, size_t end, const std::string& new_text);

private:
    char* buffer;
    bool is_file_open;
    std::string current_file;
    size_t cursor_position = 0;
}; 