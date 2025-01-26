#include "groq_client.hpp"
#include <iostream>
#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Change the global WriteCallback to be a member function
size_t GroqClient::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string GroqClient::makeRequest(const std::string& endpoint, const json& request_data) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    try {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key;
        headers = curl_slist_append(headers, auth_header.c_str());

        std::string url = api_endpoint + endpoint;
        std::string request_body = request_data.dump();

        // Debug output
        if (debug_mode) {
            std::cout << "Request URL: " << url << std::endl;
            std::cout << "Request body: " << request_body << std::endl;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        if (debug_mode) {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
        }

        return response;

    } catch (const std::exception& e) {
        if (curl) curl_easy_cleanup(curl);
        throw;
    }
}

std::string GroqClient::getCompletion(const std::string& prompt) {
    try {
        json request_data = {
            {"model", "mixtral-8x7b-32768"},
            {"messages", json::array({
                {
                    {"role", "system"},
                    {"content", "You are a helpful programming assistant."}
                },
                {
                    {"role", "user"},
                    {"content", prompt}
                }
            })},
            {"temperature", 0.7},
            {"max_tokens", 2048},
            {"stream", false}
        };

        std::string response = makeRequest("chat/completions", request_data);
        
        if (debug_mode) {
            std::cout << "Response received, length: " << response.length() << std::endl;
        }

        json response_json = json::parse(response);
        
        if (response_json.contains("choices") && 
            !response_json["choices"].empty() && 
            response_json["choices"][0].contains("message") &&
            response_json["choices"][0]["message"].contains("content")) {
            
            std::string content = response_json["choices"][0]["message"]["content"].get<std::string>();
            
            if (!content.empty() && debug_mode) {
                std::cout << "Successfully extracted content, length: " << content.length() << std::endl;
            }
            return content;
        }

        return "Error: Invalid response format";

    } catch (const std::exception& e) {
        std::cerr << "Error in getCompletion: " << e.what() << std::endl;
        return "Error: " + std::string(e.what());
    }
}

std::vector<float> GroqClient::getEmbedding(const std::string& text) {
    json request_data = {
        {"model", "mixtral-8x7b-32768"},
        {"input", text},
        {"encoding_format", "float"}
    };

    std::string response = makeRequest("/embeddings", request_data);
    json response_json = json::parse(response);
    
    if (response_json.contains("data") && 
        !response_json["data"].empty() && 
        response_json["data"][0].contains("embedding")) {
        return response_json["data"][0]["embedding"].get<std::vector<float>>();
    }
    
    throw std::runtime_error("Failed to get embedding from API");
}
