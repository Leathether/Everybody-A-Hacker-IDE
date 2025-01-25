#pragma once

#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class GroqClient {
private:
    std::string api_key;
    const std::string api_endpoint = "https://api.groq.com/openai/v1/chat/completions";
    
    // Add debug flag
    bool debug_mode = true;

public:
    GroqClient(const std::string& key) : api_key(key) {}
    std::string getCompletion(const std::string& prompt);
    
    // Add debug mode setter
    void setDebugMode(bool debug) { debug_mode = debug; }
}; 