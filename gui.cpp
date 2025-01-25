// Include necessary headers
#include "cursor_clone.hpp"

void CursorClone::renderGUI() {
    // Move GUI rendering code here

    // Terminal section with unique ID
    ImGui::PushID("MainTerminal");  // Add a unique ID scope
    ImGui::BeginChild("Terminal", ImVec2(0, 0), true);
    {
        // Terminal output area with unique ID
        float terminal_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        ImGui::PushID("Output");  // Add another unique ID scope
        ImGui::BeginChild("TerminalOutput", ImVec2(0, terminal_height), true, 
            ImGuiWindowFlags_HorizontalScrollbar | 
            ImGuiWindowFlags_AlwaysHorizontalScrollbar);

        // ... terminal output rendering code ...

        ImGui::EndChild();
        ImGui::PopID();  // Pop Output ID scope

        // Prompt section with unique ID
        ImGui::PushID("Prompt");
        
        // Ubuntu-style terminal prompt
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(87, 227, 137, 255));  // Ubuntu green
        ImGui::Text("user@%s:", getHostname().c_str());  // Username and hostname
        ImGui::SameLine();
        
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(87, 207, 227, 255));  // Light blue for path
        ImGui::Text("%s", getDisplayPath(current_directory).c_str());
        ImGui::SameLine();
        
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(227, 87, 87, 255));  // Red for $
        ImGui::Text("$");
        ImGui::SameLine();
        ImGui::PopStyleColor(3);  // Pop all colors

        // Terminal input with unique ID
        ImGui::PushItemWidth(-1);
        if (ImGui::InputText("##TerminalInputField", terminal_buffer, sizeof(terminal_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion,
            terminalInputCallback, this)) {
            // ... input handling code ...
        }
        ImGui::PopItemWidth();
        
        ImGui::PopID();  // Pop Prompt ID scope
    }
    ImGui::EndChild();
    ImGui::PopID();  // Pop MainTerminal ID scope

    // ... rest of GUI code ...
}

void CursorClone::setupFonts() {
    // Move font setup code here
}

void CursorClone::showBrowseButton() {
    // Move browse button code here
} 