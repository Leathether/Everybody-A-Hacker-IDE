// Include necessary headers
#include "cursor_clone.hpp"

void CursorClone::executeTerminalCommand(const std::string& command) {
    // Move all terminal command execution code here
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

        async_command->future = std::async(std::launch::async, [this, command]() {
            try {
                // First change the actual working directory
                if (chdir(current_directory.c_str()) != 0) {
                    std::lock_guard<std::mutex> lock(async_command->output_mutex);
                    async_command->output_buffer.push_back("Error: Could not change to directory: " + current_directory);
                    async_command->running = false;
                    return;
                }

                std::string full_command;
                #ifdef _WIN32
                    // Windows handling...
                #else
                    // For Unix systems, handle Python commands specially
                    if (command.find("python") != std::string::npos) {
                        std::string actual_command;
                        std::string script_name;
                        
                        // Parse the command to get the script name
                        if (command.substr(0, 7) == "python ") {
                            script_name = command.substr(7);
                        } else if (command.substr(0, 8) == "python3 ") {
                            script_name = command.substr(8);
                        } else if (command.length() > 3 && command.substr(command.length() - 3) == ".py") {
                            script_name = command;
                        }

                        // Remove any quotes that might be present
                        script_name.erase(std::remove(script_name.begin(), script_name.end(), '\"'), script_name.end());
                        
                        // Build the command
                        full_command = "python3 " + script_name;
                    } else {
                        full_command = command;
                    }
                    full_command = "bash -c '" + full_command + "' 2>&1";
                #endif

                // Execute command
                #ifdef _WIN32
                    async_command->pipe = _popen(full_command.c_str(), "r");
                #else
                    async_command->pipe = popen(full_command.c_str(), "r");
                #endif

                // Rest of the command execution code...
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(async_command->output_mutex);
                async_command->output_buffer.push_back("Error: " + std::string(e.what()));
                async_command->running = false;
            }
        });
    } catch (const std::exception& e) {
        terminal_history.push_back("Error launching command: " + std::string(e.what()));
        if (async_command) async_command->running = false;
    }
}

void CursorClone::checkCommandOutput() {
    // Move command output checking code here
}

void CursorClone::navigateCommandHistory(bool up) {
    // Move command history navigation code here
}

int CursorClone::terminalInputCallback(ImGuiInputTextCallbackData* data) {
    // Move terminal input callback code here
} 