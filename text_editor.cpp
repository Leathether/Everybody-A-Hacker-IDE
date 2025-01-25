#include "text_editor.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>

void TextEditor::openFile(const std::string& path) {
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate); // Open at end to get size
        if (!file.is_open()) {
            std::cerr << "Could not open file: " << path << std::endl;
            return;
        }

        // Get file size and check if it's too large
        std::streamsize size = file.tellg();
        if (size >= BUFFER_SIZE - 1) {
            std::cerr << "File too large for buffer (max " << BUFFER_SIZE << " bytes)" << std::endl;
            return;
        }

        // Read file content
        file.seekg(0, std::ios::beg);
        std::vector<char> temp_buffer(size + 1);
        if (!file.read(temp_buffer.data(), size)) {
            std::cerr << "Error reading file: " << path << std::endl;
            return;
        }
        temp_buffer[size] = '\0';

        // Copy to buffer only after successful read
        std::memset(buffer, 0, BUFFER_SIZE);
        std::memcpy(buffer, temp_buffer.data(), size + 1);
        current_file = path;
        is_file_open = true;

    } catch (const std::exception& e) {
        std::cerr << "Exception while opening file: " << e.what() << std::endl;
        is_file_open = false;
        current_file.clear();
        buffer[0] = '\0';
    }
}

void TextEditor::saveFile() {
    if (!is_file_open || current_file.empty()) return;

    try {
        std::ofstream file(current_file, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Could not save file: " << current_file << std::endl;
            return;
        }

        file.write(buffer, strlen(buffer));
        file.close();

    } catch (const std::exception& e) {
        std::cerr << "Exception while saving file: " << e.what() << std::endl;
    }
}

bool TextEditor::render(const ImVec2& size) {
    bool content_changed = false;
    
    if (is_file_open) {
        static ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | 
                                         ImGuiInputTextFlags_CtrlEnterForNewLine;
        
        // Callback to capture cursor position
        static auto callback = [](ImGuiInputTextCallbackData* data) -> int {
            TextEditor* editor = (TextEditor*)data->UserData;
            editor->setCursorPosition(data->CursorPos);
            return 0;
        };
        
        content_changed = ImGui::InputTextMultiline(
            "##editor",
            buffer,
            BUFFER_SIZE,
            size,
            flags | ImGuiInputTextFlags_CallbackAlways,
            callback,
            (void*)this  // Pass this pointer to access it in callback
        );

        // Show file name and cursor position in the editor
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        ImGui::Text("File: %s (Cursor: %zu)", current_file.c_str(), cursor_position);
    } else {
        ImGui::TextWrapped("No file open");
    }

    return content_changed;
}

void TextEditor::setContent(const std::string& content) {
    if (content.length() >= BUFFER_SIZE) {
        std::cerr << "Content too large for buffer" << std::endl;
        return;
    }
    std::strncpy(buffer, content.c_str(), BUFFER_SIZE - 1);
    buffer[BUFFER_SIZE - 1] = '\0';
}

void TextEditor::insertAtCursor(const std::string& text) {
    if (!is_file_open) return;
    
    size_t current_len = strlen(buffer);
    size_t insert_len = text.length();
    
    // Check if we have enough space
    if (current_len + insert_len >= BUFFER_SIZE - 1) {
        std::cerr << "Not enough space to insert text" << std::endl;
        return;
    }
    
    // Make space for new text by moving existing text after cursor
    memmove(buffer + cursor_position + insert_len, 
            buffer + cursor_position, 
            current_len - cursor_position + 1);  // +1 for null terminator
            
    // Insert the new text at cursor position
    memcpy(buffer + cursor_position, text.c_str(), insert_len);
    
    // Update cursor position
    cursor_position += insert_len;
}

void TextEditor::replaceText(size_t start, size_t end, const std::string& new_text) {
    if (!is_file_open || start > end || end > strlen(buffer)) {
        return;
    }
    
    size_t old_len = end - start;
    size_t new_len = new_text.length();
    size_t total_len = strlen(buffer);
    
    // Check if we have enough space
    if (total_len - old_len + new_len >= BUFFER_SIZE - 1) {
        std::cerr << "Not enough space to replace text" << std::endl;
        return;
    }
    
    // Move existing text to make room (or remove space) for new text
    memmove(buffer + start + new_len, 
            buffer + end, 
            total_len - end + 1);  // +1 for null terminator
            
    // Insert the new text
    memcpy(buffer + start, new_text.c_str(), new_len);
    
    // Update cursor position if needed
    if (cursor_position > start) {
        if (cursor_position < end) {
            cursor_position = start + new_len;  // Move cursor to end of inserted text
        } else {
            cursor_position = cursor_position - old_len + new_len;  // Adjust for size difference
        }
    }
} 