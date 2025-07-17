/**
 * MailSlot vs Pipe IPC Comparison Test
 * 
 * This standalone test demonstrates why MailSlots are not suitable for 
 * replacing the current pipe-based IPC mechanism in Tardsplaya.
 * 
 * Compile with: cl /EHsc mailslot_test.cpp mailslot_comparison.cpp
 */

#include <windows.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include "mailslot_comparison.h"

// Simple debug logging for standalone test
void AddDebugLog(const std::wstring& message) {
    std::wcout << L"[LOG] " << message << std::endl;
}

int main() {
    std::wcout << L"=== MailSlot vs Pipe IPC Comparison Test ===" << std::endl;
    std::wcout << L"Testing feasibility of replacing pipes with MailSlots for video streaming..." << std::endl << std::endl;
    
    // Test with different video segment sizes
    std::vector<size_t> test_sizes = {
        64 * 1024,      // 64KB - at MailSlot limit
        256 * 1024,     // 256KB - typical small segment
        1024 * 1024,    // 1MB - typical medium segment
        5 * 1024 * 1024 // 5MB - large segment
    };
    
    for (size_t test_size : test_sizes) {
        std::wcout << L"\n--- Testing with " << test_size / 1024 << L"KB video segment ---" << std::endl;
        
        // Create test video data
        std::vector<char> video_data(test_size);
        for (size_t i = 0; i < test_size; ++i) {
            video_data[i] = (char)(i % 256);
        }
        
        // Test MailSlot approach
        std::atomic<bool> cancel_token{false};
        std::wstring mailslot_name = L"\\\\.\\mailslot\\tardsplaya_ipc_test";
        
        auto mailslot_result = TestMailSlotDataTransfer(video_data, mailslot_name, cancel_token);
        
        // Test Pipe approach
        auto pipe_start = std::chrono::high_resolution_clock::now();
        HANDLE hRead, hWrite;
        SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const DWORD PIPE_BUFFER_SIZE = 1024 * 1024; // 1MB
        
        bool pipe_success = false;
        if (CreatePipe(&hRead, &hWrite, &sa, PIPE_BUFFER_SIZE)) {
            DWORD bytes_written = 0;
            pipe_success = WriteFile(hWrite, video_data.data(), (DWORD)video_data.size(), &bytes_written, nullptr);
            CloseHandle(hRead);
            CloseHandle(hWrite);
        }
        auto pipe_end = std::chrono::high_resolution_clock::now();
        double pipe_time = std::chrono::duration<double, std::milli>(pipe_end - pipe_start).count();
        
        // Display results
        std::wcout << L"  MailSlot: " << (mailslot_result.success ? L"SUCCESS" : L"FAILED") 
                   << L" - " << mailslot_result.messages_sent << L" messages, "
                   << std::fixed << std::setprecision(2) << mailslot_result.time_taken_ms << L"ms" << std::endl;
        
        std::wcout << L"  Pipe:     " << (pipe_success ? L"SUCCESS" : L"FAILED")
                   << L" - 1 message, "
                   << std::fixed << std::setprecision(2) << pipe_time << L"ms" << std::endl;
        
        if (!mailslot_result.success) {
            std::wcout << L"  Error: " << mailslot_result.error_message << std::endl;
        }
    }
    
    // Generate comprehensive report
    std::wcout << L"\n=== COMPREHENSIVE ANALYSIS ===" << std::endl;
    
    // Test with 2MB segment (typical Twitch segment size)
    std::vector<char> typical_segment(2 * 1024 * 1024);
    for (size_t i = 0; i < typical_segment.size(); ++i) {
        typical_segment[i] = (char)(i % 256);
    }
    
    std::atomic<bool> cancel{false};
    auto result = TestMailSlotDataTransfer(typical_segment, L"\\\\.\\mailslot\\tardsplaya_final_test", cancel);
    auto report = GenerateComparisonReport(result, 1024 * 1024, true);
    
    std::wcout << report << std::endl;
    
    // Save report to file
    std::wofstream file(L"mailslot_vs_pipe_analysis.txt");
    if (file.is_open()) {
        file << report;
        file.close();
        std::wcout << L"\nDetailed analysis saved to: mailslot_vs_pipe_analysis.txt" << std::endl;
    }
    
    std::wcout << L"\nPress Enter to exit..." << std::endl;
    std::cin.get();
    
    return 0;
}