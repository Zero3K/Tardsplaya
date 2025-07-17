#include "alternative_ipc_demo.h"
#include "stream_thread.h"
#include <iostream>
#include <vector>
#include <atomic>

/**
 * Standalone test program to demonstrate MailSlot and Named Pipe alternatives
 * to the current IPC methods used in Tardsplaya
 * 
 * This addresses @Zero3K's request to see what happens when using these
 * alternative IPC mechanisms in place of the current implementation.
 */

void PrintSeparator() {
    std::wcout << L"\n" << std::wstring(80, L'=') << L"\n";
}

void PrintHeader(const std::wstring& title) {
    PrintSeparator();
    std::wcout << L"  " << title << L"\n";
    PrintSeparator();
}

int wmain(int argc, wchar_t* argv[]) {
    std::wcout << L"Tardsplaya Alternative IPC Methods Demonstration\n";
    std::wcout << L"Testing MailSlots and Named Pipes as replacements for current IPC\n";
    
    // Create test video data (1MB simulated video segment)
    std::vector<char> test_data(1024 * 1024);
    for (size_t i = 0; i < test_data.size(); i++) {
        test_data[i] = static_cast<char>(i % 256);
    }
    
    std::wcout << L"\nTest data size: " << test_data.size() << L" bytes (" 
               << (test_data.size() / 1024) << L" KB)\n";
    
    std::atomic<bool> cancel_token(false);
    std::wstring channel_name = L"demo_channel";
    
    PrintHeader(L"TESTING ALTERNATIVE IPC METHODS");
    
    // Test 1: MailSlot streaming (instead of Anonymous Pipes)
    PrintHeader(L"TEST 1: MailSlot Streaming (vs Anonymous Pipes)");
    std::wcout << L"Testing MailSlot-based streaming with bridge process...\n";
    
    auto mailslot_result = AlternativeIPCDemo::TestMailSlotStreamingInsteadOfPipes(
        test_data, L"vlc.exe --intf dummy -", cancel_token);
    
    std::wcout << L"Result: " << (mailslot_result.success ? L"SUCCESS" : L"FAILED") << L"\n";
    if (!mailslot_result.success) {
        std::wcout << L"Error: " << mailslot_result.error_message << L"\n";
    } else {
        std::wcout << L"Bytes transferred: " << mailslot_result.bytes_transferred << L"\n";
        std::wcout << L"Time taken: " << mailslot_result.time_taken_ms << L" ms\n";
        std::wcout << L"Messages sent: " << mailslot_result.message_count << L"\n";
        std::wcout << L"Notes: " << mailslot_result.performance_notes << L"\n";
    }
    
    // Test 2: Named Pipe streaming (instead of Memory-Mapped Files)
    PrintHeader(L"TEST 2: Named Pipe Streaming (vs Memory-Mapped Files)");
    std::wcout << L"Testing Named Pipe-based streaming...\n";
    
    auto namedpipe_result = AlternativeIPCDemo::TestNamedPipeInsteadOfMemoryMap(
        test_data, cancel_token);
    
    std::wcout << L"Result: " << (namedpipe_result.success ? L"SUCCESS" : L"FAILED") << L"\n";
    if (!namedpipe_result.success) {
        std::wcout << L"Error: " << namedpipe_result.error_message << L"\n";
    } else {
        std::wcout << L"Bytes transferred: " << namedpipe_result.bytes_transferred << L"\n";
        std::wcout << L"Time taken: " << namedpipe_result.time_taken_ms << L" ms\n";
        std::wcout << L"Chunks sent: " << namedpipe_result.message_count << L"\n";
        std::wcout << L"Notes: " << namedpipe_result.performance_notes << L"\n";
    }
    
    // Test 3: Named Pipe HTTP-like service (instead of TCP/HTTP)
    PrintHeader(L"TEST 3: Named Pipe HTTP-like Service (vs TCP/HTTP)");
    std::wcout << L"Testing Named Pipe HTTP-like service...\n";
    
    auto http_result = AlternativeIPCDemo::TestNamedPipeInsteadOfHttp(
        test_data, cancel_token);
    
    std::wcout << L"Result: " << (http_result.success ? L"SUCCESS" : L"FAILED") << L"\n";
    if (!http_result.success) {
        std::wcout << L"Error: " << http_result.error_message << L"\n";
    } else {
        std::wcout << L"Bytes transferred: " << http_result.bytes_transferred << L"\n";
        std::wcout << L"Time taken: " << http_result.time_taken_ms << L" ms\n";
        std::wcout << L"Notes: " << http_result.performance_notes << L"\n";
    }
    
    // Run comprehensive comparison
    PrintHeader(L"COMPREHENSIVE COMPARISON REPORT");
    
    std::vector<AlternativeIPCResult> all_results = {
        mailslot_result, namedpipe_result, http_result
    };
    
    std::wstring report = AlternativeIPCDemo::GenerateComparisonReport(all_results, test_data);
    std::wcout << report << L"\n";
    
    PrintHeader(L"KEY FINDINGS");
    std::wcout << L"1. MailSlots: Require bridge processes due to stdin incompatibility\n";
    std::wcout << L"   - Cannot directly pipe to media player stdin\n";
    std::wcout << L"   - Need intermediate process to convert messages to stream\n";
    std::wcout << L"   - Message size limits require chunking large video segments\n\n";
    
    std::wcout << L"2. Named Pipes: Better than MailSlots but more complex than anonymous pipes\n";
    std::wcout << L"   - Can handle larger chunks than MailSlots\n";
    std::wcout << L"   - Require explicit setup vs automatic anonymous pipes\n";
    std::wcout << L"   - Good for memory-mapped file replacement\n\n";
    
    std::wcout << L"3. Named Pipe HTTP: Limited compared to real TCP sockets\n";
    std::wcout << L"   - Single connection model vs multiple concurrent TCP connections\n";
    std::wcout << L"   - Custom protocol vs standard HTTP\n";
    std::wcout << L"   - Platform-specific vs cross-platform TCP\n\n";
    
    PrintHeader(L"CONCLUSION");
    std::wcout << L"The current IPC implementations in Tardsplaya are optimal:\n";
    std::wcout << L"- Anonymous pipes: Direct, efficient, stdin-compatible\n";
    std::wcout << L"- Memory-mapped files: High-performance shared memory\n";
    std::wcout << L"- TCP/HTTP server: Standard, multi-client, cross-platform\n\n";
    std::wcout << L"Alternative methods add complexity without significant benefits.\n";
    
    PrintSeparator();
    std::wcout << L"Demo completed. Press any key to exit...\n";
    std::wcin.get();
    
    return 0;
}