#pragma once

#include <string>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "groq_client.hpp"
#include <unordered_map>
#include <chrono>

class PineconeClient {
private:
    std::string pinecone_key;
    std::string pinecone_env;  // Add environment as member variable
    GroqClient& groq_client;  // For model interactions
    static constexpr const char* INDEX_NAME = "directory-index";
    static constexpr int VECTOR_DIMENSION = 1536;
    CURL* curl;
    
    // Add embedding endpoint constants
    static constexpr const char* EMBEDDING_ENDPOINT = "vectors/embed";  // Pinecone embedding endpoint
    
    // Add embedding cache
    struct CachedEmbedding {
        std::vector<float> embedding;
        std::chrono::system_clock::time_point timestamp;
    };
    std::unordered_map<std::string, CachedEmbedding> embedding_cache;
    static constexpr size_t MAX_CACHE_SIZE = 1000;
    static constexpr int CACHE_TTL_SECONDS = 3600; // 1 hour cache lifetime
    
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);
    std::string makeRequest(const std::string& endpoint, const std::string& method, const nlohmann::json& data = nullptr);
    std::string makeControlRequest(const std::string& endpoint, const std::string& method, const nlohmann::json& data = nullptr);
    bool indexExists();
    void createIndex();
    void waitForIndexReady();
    
    // Add cache management methods
    std::vector<float> getCachedEmbedding(const std::string& text);
    void cacheEmbedding(const std::string& text, const std::vector<float>& embedding);
    void cleanCache();

    // Add method to get embeddings from Pinecone
    std::vector<float> getPineconeEmbedding(const std::string& text);

public:
    PineconeClient(const std::string& pinecone_key, const std::string& pinecone_env, GroqClient& groq_client);
    ~PineconeClient();
    
    void initializeIndex();  // New public method to handle index creation
    void upsertText(const std::string& id, const std::string& text, const nlohmann::json& metadata);
    void queryText(const std::string& text, int top_k = 10);
    void deleteVector(const std::string& id);
}; 