#pragma once
#include <string>
#include <sstream>
#include <cctype>

// Decode HTTP chunked transfer encoding
inline std::string decode_chunked_body(const std::string& chunked_data) {
    std::string result;
    size_t pos = 0;
    
    while (pos < chunked_data.size()) {
        // Find end of chunk size line
        size_t size_end = chunked_data.find("\r\n", pos);
        if (size_end == std::string::npos) break;
        
        // Parse chunk size (hex)
        std::string size_str = chunked_data.substr(pos, size_end - pos);
        // Remove any chunk extensions (everything after semicolon)
        size_t semi = size_str.find(';');
        if (semi != std::string::npos) {
            size_str = size_str.substr(0, semi);
        }
        
        size_t chunk_size = 0;
        try {
            chunk_size = std::stoul(size_str, nullptr, 16);
        } catch (...) {
            break; // Invalid chunk size
        }
        
        // If chunk size is 0, we've reached the end
        if (chunk_size == 0) break;
        
        // Move past the size line
        pos = size_end + 2;
        
        // Check if we have enough data for the chunk
        if (pos + chunk_size + 2 > chunked_data.size()) break;
        
        // Extract chunk data
        result.append(chunked_data.substr(pos, chunk_size));
        
        // Move past chunk data and trailing CRLF
        pos += chunk_size + 2;
    }
    
    return result;
}