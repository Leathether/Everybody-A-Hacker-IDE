#include "pinecone_client.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

PineconeClient::PineconeClient(const std::string& pinecone_key, const std::string& pinecone_env, GroqClient& groq_client)
    : pinecone_key(pinecone_key), pinecone_env(pinecone_env), groq_client(groq_client) {
    
    // Initialize CURL
    curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    // Test connectivity before proceeding
    try {
        // Test connection to Pinecone API
        std::string test_url = "https://controller." + pinecone_env + ".pinecone.io/actions/whoami";
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Api-Key: " + pinecone_key).c_str());
        
        curl_easy_setopt(curl, CURLOPT_URL, test_url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("Failed to connect to Pinecone API: " + 
                std::string(curl_easy_strerror(res)));
        }

        // Initialize the index
        initializeIndex();
    } catch (const std::exception& e) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Pinecone API Error: " + std::string(e.what()) + 
            "\nPlease check your API key and environment settings.");
    }
}

PineconeClient::~PineconeClient() {
    if (curl) {
        curl_easy_cleanup(curl);
    }
}

size_t PineconeClient::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string PineconeClient::makeRequest(const std::string& endpoint, const std::string& method, const nlohmann::json& data) {
    std::string url = "https://" + std::string(INDEX_NAME) + "-" + 
                     pinecone_env + ".svc.pinecone.io/vectors/" + endpoint;
    std::string response;
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Api-Key: " + pinecone_key).c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    if (method == "POST" && !data.is_null()) {
        std::string json_str = data.dump();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    }
    
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(res)));
    }
    
    return response;
}

std::vector<float> PineconeClient::getPineconeEmbedding(const std::string& text) {
    nlohmann::json request_data = {
        {"text", text}
    };

    std::string url = "https://" + std::string(INDEX_NAME) + "-" + 
                     pinecone_env + ".svc.pinecone.io/" + EMBEDDING_ENDPOINT;
    std::string response;
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Api-Key: " + pinecone_key).c_str());
    
    std::string request_body = request_data.dump();
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("Failed to get embedding from Pinecone: " + 
            std::string(curl_easy_strerror(res)));
    }
    
    auto json_response = nlohmann::json::parse(response);
    return json_response["embedding"].get<std::vector<float>>();
}

std::vector<float> PineconeClient::getCachedEmbedding(const std::string& text) {
    auto now = std::chrono::system_clock::now();
    auto it = embedding_cache.find(text);
    
    if (it != embedding_cache.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.timestamp).count();
            
        if (age < CACHE_TTL_SECONDS) {
            return it->second.embedding;
        } else {
            embedding_cache.erase(it);
        }
    }
    
    if (embedding_cache.size() >= MAX_CACHE_SIZE) {
        cleanCache();
    }
    
    // Use Pinecone's embedding service
    auto embedding = getPineconeEmbedding(text);
    cacheEmbedding(text, embedding);
    return embedding;
}

void PineconeClient::cacheEmbedding(const std::string& text, const std::vector<float>& embedding) {
    CachedEmbedding cached;
    cached.embedding = embedding;
    cached.timestamp = std::chrono::system_clock::now();
    embedding_cache[text] = cached;
}

void PineconeClient::cleanCache() {
    auto now = std::chrono::system_clock::now();
    
    // Remove expired entries
    for (auto it = embedding_cache.begin(); it != embedding_cache.end();) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.timestamp).count();
            
        if (age >= CACHE_TTL_SECONDS) {
            it = embedding_cache.erase(it);
        } else {
            ++it;
        }
    }
    
    // If still too large, remove oldest entries
    if (embedding_cache.size() >= MAX_CACHE_SIZE) {
        std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> entries;
        for (const auto& entry : embedding_cache) {
            entries.push_back({entry.first, entry.second.timestamp});
        }
        
        // Sort by timestamp
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) {
                return a.second < b.second;
            });
            
        // Remove oldest entries until we're under the limit
        size_t to_remove = embedding_cache.size() - (MAX_CACHE_SIZE * 0.8); // Remove 20% of max size
        for (size_t i = 0; i < to_remove && i < entries.size(); ++i) {
            embedding_cache.erase(entries[i].first);
        }
    }
}

void PineconeClient::upsertText(const std::string& id, const std::string& text, const nlohmann::json& metadata) {
    auto embedding = getCachedEmbedding(text);
    
    nlohmann::json data = {
        {"vectors", {{
            {"id", id},
            {"values", embedding},
            {"metadata", metadata}
        }}}
    };
    
    makeRequest("upsert", "POST", data);
}

void PineconeClient::queryText(const std::string& text, int top_k) {
    auto embedding = getCachedEmbedding(text);
    
    nlohmann::json data = {
        {"vector", embedding},
        {"top_k", top_k},
        {"include_metadata", true}
    };
    
    makeRequest("query", "POST", data);
}

void PineconeClient::deleteVector(const std::string& id) {
    nlohmann::json data = {
        {"ids", {id}}
    };
    
    makeRequest("delete", "POST", data);
}

std::string PineconeClient::makeControlRequest(const std::string& endpoint, const std::string& method, const nlohmann::json& data) {
    std::string url = "https://controller.pinecone.io/" + endpoint;
    std::string response;
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Api-Key: " + pinecone_key).c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // 30 second timeout
    
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!data.is_null()) {
            std::string json_str = data.dump();
            // Only log non-sensitive request data
            std::cout << "Making " << method << " request to endpoint: " << endpoint << std::endl;
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        }
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(res)) + 
            "\nURL: " + url);
    }
    
    // Only log that the request was successful, not the actual response which may contain sensitive data
    std::cout << "Request to " << endpoint << " completed successfully" << std::endl;
    return response;
}

bool PineconeClient::indexExists() {
    try {
        std::string response = makeControlRequest("databases", "GET");
        auto json_response = nlohmann::json::parse(response);
        
        for (const auto& index : json_response) {
            if (index["name"] == INDEX_NAME) {
                return true;
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error checking index existence: " << e.what() << std::endl;
        return false;
    }
}

void PineconeClient::createIndex() {
    nlohmann::json create_request = {
        {"name", INDEX_NAME},
        {"dimension", VECTOR_DIMENSION},
        {"metric", "cosine"},
        {"spec", {
            {"serverless", {
                {"cloud", "aws"},
                {"region", "us-west-2"}
            }},
            {"model", "mixtral-8x7b-32768"}  // Specify the Groq model
        }}
    };
    
    try {
        makeControlRequest("databases", "POST", create_request);
        std::cout << "Creating Pinecone index '" << INDEX_NAME << "' with Groq model..." << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to create index: " + std::string(e.what()));
    }
}

void PineconeClient::waitForIndexReady() {
    int attempts = 0;
    const int max_attempts = 30;  // Wait up to 5 minutes
    
    while (attempts < max_attempts) {
        try {
            std::string response = makeControlRequest("databases/" + std::string(INDEX_NAME), "GET");
            auto json_response = nlohmann::json::parse(response);
            
            if (json_response["status"]["ready"] == true) {
                std::cout << "Pinecone index is ready!" << std::endl;
                return;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error checking index status: " << e.what() << std::endl;
        }
        
        std::cout << "Waiting for index to be ready..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(10));
        attempts++;
    }
    
    throw std::runtime_error("Timeout waiting for index to be ready");
}

void PineconeClient::initializeIndex() {
    if (!indexExists()) {
        std::cout << "Pinecone index '" << INDEX_NAME << "' does not exist. Creating..." << std::endl;
        createIndex();
        waitForIndexReady();
    } else {
        std::cout << "Pinecone index '" << INDEX_NAME << "' already exists." << std::endl;
    }
} 