// Include necessary headers
#include "cursor_clone.hpp"
#include <fstream>
#include <string>
#include <memory>
#include <future>
#include <deque>
#include <mutex>
#include <chrono>
#include <thread>

void CursorClone::executeTerminalCommand(const std::string& command) {
    try {
        // Check if a command is already running
        if (async_command && async_command->running) {
            terminal_history.push_back("Error: A command is already running. Please wait for it to finish.");
            return;
        }

        // Add command to history
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

        // Handle cd command specially
        if (command.substr(0, 3) == "cd ") {
            std::string path = command.substr(3);
            path.erase(0, path.find_first_not_of(" \t\r\n"));
            path.erase(path.find_last_not_of(" \t\r\n") + 1);
            
            if (path.empty() || path == "~") {
                changeDirectory(getHomeDirectory());
            } else {
                changeDirectory(path);
            }
            return;
        }

        // Execute the command
        terminal_history.push_back(getUsername() + "@" + getHostname() + ":" + getDisplayPath(current_directory) + "$ " + command);
        executeCommandAsync(command);
        
        // Wait for command to complete while processing output
        while (async_command && async_command->running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            checkCommandOutput();
        }

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
                    if (command.find("python") != std::string::npos || command.find(".py") != std::string::npos) {
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
                        
                        // Build the command with error handling
                        full_command = "python3 " + script_name + " 2>&1";
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

                if (!async_command->pipe) {
                    std::lock_guard<std::mutex> lock(async_command->output_mutex);
                    async_command->output_buffer.push_back("Error: Failed to execute command");
                    async_command->running = false;
                    return;
                }

                char buffer[4096];
                while (fgets(buffer, sizeof(buffer), async_command->pipe) != nullptr) {
                    std::lock_guard<std::mutex> lock(async_command->output_mutex);
                    async_command->output_buffer.push_back(std::string(buffer));
                }

                // Close the pipe and get the exit status
                int status = pclose(async_command->pipe);
                async_command->pipe = nullptr;

                if (status != 0) {
                    std::lock_guard<std::mutex> lock(async_command->output_mutex);
                    async_command->output_buffer.push_back("Command exited with status " + std::to_string(status));
                }

                async_command->running = false;
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
    if (!async_command || !async_command->running) return;

    std::lock_guard<std::mutex> lock(async_command->output_mutex);
    while (!async_command->output_buffer.empty()) {
        terminal_history.push_back(async_command->output_buffer.front());
        async_command->output_buffer.pop_front();
    }
}

void CursorClone::navigateCommandHistory(bool up) {
    // Move command history navigation code here
}

int CursorClone::terminalInputCallback(ImGuiInputTextCallbackData* data) {
    // Move terminal input callback code here
} 