#pragma once

#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class GroqClient {
private:
    std::string api_key;
    const std::string api_endpoint = "https://api.groq.com/openai/v1/";  // Updated base endpoint
    
    // Add debug flag
    bool debug_mode = true;

    // Add makeRequest method declaration
    std::string makeRequest(const std::string& endpoint, const json& request_data);
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);

public:
    GroqClient(const std::string& key) : api_key(key) {}
    std::string getCompletion(const std::string& prompt);
    
    // Add debug mode setter
    void setDebugMode(bool debug) { debug_mode = debug; }

    std::vector<float> getEmbedding(const std::string& text);
}; 