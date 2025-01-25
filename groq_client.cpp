#include "groq_client.hpp"
#include <iostream>
#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Callback function to handle CURL response
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string GroqClient::getCompletion(const std::string& prompt) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (!curl) {
        return "Error: Failed to initialize CURL";
    }

    try {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key;
        headers = curl_slist_append(headers, auth_header.c_str());

        // Create the request payload with the correct format
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

        std::string request_body = request_data.dump();

        // Debug output
        std::cout << "Request URL: " << api_endpoint << std::endl;
        std::cout << "Request body: " << request_body << std::endl;

        // Set CURL options
        curl_easy_setopt(curl, CURLOPT_URL, api_endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        // Enable verbose debug output
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        // Perform the request
        CURLcode res = curl_easy_perform(curl);

        // Clean up
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
            return "Error: " + std::string(curl_easy_strerror(res));
        }

        // Debug output
        std::cout << "Raw response: " << response << std::endl;

        // Parse and extract the response
        json response_json = json::parse(response);
        
        // Check for API errors
        if (response_json.contains("error")) {
            std::cerr << "API error: " << response_json["error"].dump(2) << std::endl;
            return "Error: " + response_json["error"]["message"].get<std::string>();
        }

        // Extract the response content
        if (response_json.contains("choices") && 
            !response_json["choices"].empty() && 
            response_json["choices"][0].contains("message") &&
            response_json["choices"][0]["message"].contains("content")) {
            
            return response_json["choices"][0]["message"]["content"].get<std::string>();
        }

        return "Error: Unexpected response format";

    } catch (const json::exception& e) {
        std::cerr << "JSON error: " << e.what() << std::endl;
        return "Error parsing response: " + std::string(e.what());
    } catch (const std::exception& e) {
        std::cerr << "General error: " << e.what() << std::endl;
        return "Error: " + std::string(e.what());
    }
}
