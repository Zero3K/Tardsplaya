#include "gpac_decoder.h"
#include <iostream>
#include <vector>
#include <string>

// Test the real GPAC integration
int main() {
    std::wcout << L"Testing GPAC library integration..." << std::endl;
    
    // Create GPAC decoder
    gpac_decoder::GpacHLSDecoder decoder;
    
    if (!decoder.Initialize()) {
        std::wcerr << L"Failed to initialize GPAC decoder!" << std::endl;
        return 1;
    }
    
    std::wcout << L"GPAC decoder initialized successfully!" << std::endl;
    
    // Test HLS processing with a fake URL (will fail but should not crash)
    std::wstring test_url = L"https://example.com/test.m3u8";
    std::vector<uint8_t> mp4_output;
    std::wstring error_msg;
    
    std::wcout << L"Testing HLS processing (expected to fail with connection error)..." << std::endl;
    
    bool success = decoder.ProcessHLS(test_url, mp4_output, error_msg);
    
    if (!success) {
        std::wcout << L"HLS processing failed as expected: " << error_msg << std::endl;
    } else {
        std::wcout << L"HLS processing unexpectedly succeeded: " << mp4_output.size() << L" bytes" << std::endl;
    }
    
    // Get decoder stats
    auto stats = decoder.GetStats();
    std::wcout << L"Segments processed: " << stats.segments_processed << std::endl;
    std::wcout << L"Bytes output: " << stats.bytes_output << std::endl;
    
    std::wcout << L"GPAC integration test completed!" << std::endl;
    return 0;
}

// Dummy implementation for external dependencies
std::wstring Utf8ToWide(const std::string& str) {
    return std::wstring(str.begin(), str.end());
}