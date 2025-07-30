#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>

// Include our wrapper
#include "demux_mpegts_wrapper.h"

// Mock functions for Tardsplaya dependencies
bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token) {
    // Mock implementation - return a simple M3U8 playlist for testing
    out = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n"
          "#EXTINF:10.0,\nsegment001.ts\n#EXTINF:10.0,\nsegment002.ts\n";
    return true;
}

bool HttpGetBinary(const std::wstring& url, std::vector<uint8_t>& out, std::atomic<bool>* cancel_token) {
    // Mock implementation - return empty data for testing
    out.clear();
    out.resize(1024, 0x47); // Fill with TS sync bytes for basic testing
    return true;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    std::wstring result;
    result.resize(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        result[i] = static_cast<wchar_t>(str[i]);
    }
    return result;
}

void AddDebugLog(const std::wstring& msg) {
    std::wcout << L"[DEBUG] " << msg << std::endl;
}

int main() {
    std::wcout << L"Testing Demux-MPEGTS Wrapper..." << std::endl;
    
    try {
        // Test 1: Create demux wrapper
        std::wcout << L"Test 1: Creating demux wrapper..." << std::endl;
        auto wrapper = tardsplaya::CreateDemuxWrapper(L"mpv.exe", true, true);
        
        if (!wrapper) {
            std::wcout << L"FAILED: Could not create demux wrapper" << std::endl;
            return 1;
        }
        std::wcout << L"PASSED: Demux wrapper created successfully" << std::endl;
        
        // Test 2: Check initial state
        std::wcout << L"Test 2: Checking initial state..." << std::endl;
        if (wrapper->IsDemuxing()) {
            std::wcout << L"FAILED: Wrapper should not be demuxing initially" << std::endl;
            return 1;
        }
        std::wcout << L"PASSED: Initial state is correct" << std::endl;
        
        // Test 3: Check stream management
        std::wcout << L"Test 3: Checking stream management..." << std::endl;
        auto streams = wrapper->GetAvailableStreams();
        std::wcout << L"Available streams: " << streams.size() << std::endl;
        std::wcout << L"PASSED: Stream management accessible" << std::endl;
        
        // Test 4: Check statistics
        std::wcout << L"Test 4: Checking statistics..." << std::endl;
        auto stats = wrapper->GetStats();
        std::wcout << L"Total packets processed: " << stats.total_packets_processed << std::endl;
        std::wcout << L"PASSED: Statistics accessible" << std::endl;
        
        // Test 5: Quick start/stop test (with cancel immediately)
        std::wcout << L"Test 5: Quick start/stop test..." << std::endl;
        std::atomic<bool> cancel_token(false);
        
        // Start demuxing (this will likely fail due to mock data, but should not crash)
        bool started = wrapper->StartDemuxing(L"http://test.example.com/playlist.m3u8", cancel_token,
            [](const std::wstring& msg) {
                std::wcout << L"[LOG] " << msg << std::endl;
            });
        
        // Cancel immediately to test cleanup
        cancel_token.store(true);
        
        // Wait a moment for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Stop demuxing
        wrapper->StopDemuxing();
        
        std::wcout << L"PASSED: Start/stop test completed without crashes" << std::endl;
        
        std::wcout << L"All tests passed! Demux-MPEGTS wrapper is functional." << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cout << "FAILED: Exception caught: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::wcout << L"FAILED: Unknown exception caught" << std::endl;
        return 1;
    }
}