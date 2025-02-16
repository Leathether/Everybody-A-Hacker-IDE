#pragma once

#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <functional>

using json = nlohmann::json;

class GroqClient {
private:
    std::string api_key;
    const std::string api_endpoint = "https://api.groq.com/openai/v1/";  // Updated base endpoint
    
    // Add debug flag
    bool debug_mode = true;

    // Add makeRequest method declaration
    std::string makeRequest(const std::string& endpoint, const json& request_data);
    std::string makeStreamingRequest(const std::string& endpoint, const json& request_data, 
                                   std::function<void(const std::string&)> callback);
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);
    static size_t StreamingWriteCallback(void* contents, size_t size, size_t nmemb, void* userp);

    struct StreamingContext {
        std::function<void(const std::string&)> callback;
        std::string buffer;
        std::string full_response;
    };

public:
    GroqClient(const std::string& key) : api_key(key) {}
    std::string getCompletion(const std::string& prompt);
    void getCompletionStreaming(const std::string& prompt, std::function<void(const std::string&)> callback);
    
    // Add debug mode setter
    void setDebugMode(bool debug) { debug_mode = debug; }

    std::vector<float> getEmbedding(const std::string& text);
}; 