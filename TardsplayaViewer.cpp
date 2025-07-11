#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <io.h>
#include <vector>
#include "stream_memory_map.h"

// Simple debug logging for the viewer (outputs to stderr)
void AddDebugLog(const std::wstring& msg) {
    // Convert to console output for debugging
    std::wcerr << L"[VIEWER] " << msg << std::endl;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        std::wcerr << L"Usage: TardsplayaViewer.exe <stream_name>" << std::endl;
        std::wcerr << L"This program reads stream data from a memory-mapped file and outputs to stdout" << std::endl;
        return 1;
    }

    std::wstring stream_name = argv[1];
    AddDebugLog(L"Starting viewer for stream: " + stream_name);

    // Create memory map reader
    StreamMemoryMap memory_map;
    
    // Try to open the memory map (with retries for startup timing)
    bool connected = false;
    for (int attempts = 0; attempts < 30; ++attempts) {
        if (memory_map.OpenAsReader(stream_name)) {
            connected = true;
            AddDebugLog(L"Successfully connected to memory map: " + stream_name);
            break;
        }
        
        AddDebugLog(L"Attempt " + std::to_wstring(attempts + 1) + L" to connect to memory map failed, retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    if (!connected) {
        AddDebugLog(L"Failed to connect to memory map after 30 attempts");
        return 1;
    }

    // Set stdout to binary mode to handle video data properly
    _setmode(_fileno(stdout), _O_BINARY);
    
    AddDebugLog(L"Starting data streaming to stdout...");
    
    // Buffer for reading data
    const size_t BUFFER_SIZE = 64 * 1024; // 64KB buffer
    std::vector<char> buffer(BUFFER_SIZE);
    
    size_t total_bytes_read = 0;
    int consecutive_empty_reads = 0;
    const int MAX_EMPTY_READS = 100; // 5 seconds at 50ms intervals
    
    while (true) {
        // Read data from memory map
        size_t bytes_read = memory_map.ReadData(buffer.data(), BUFFER_SIZE);
        
        if (bytes_read > 0) {
            // Write data to stdout (for media player)
            DWORD bytes_written;
            HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
            
            if (!WriteFile(stdout_handle, buffer.data(), static_cast<DWORD>(bytes_read), &bytes_written, nullptr)) {
                AddDebugLog(L"Failed to write to stdout, media player may have disconnected");
                break;
            }
            
            if (bytes_written != bytes_read) {
                AddDebugLog(L"Incomplete write to stdout: " + std::to_wstring(bytes_written) + 
                           L"/" + std::to_wstring(bytes_read) + L" bytes");
                break;
            }
            
            total_bytes_read += bytes_read;
            consecutive_empty_reads = 0;
            
            // Flush stdout to ensure data is sent immediately
            FlushFileBuffers(stdout_handle);
        } else {
            consecutive_empty_reads++;
            
            // Check if stream has ended
            if (memory_map.IsStreamEnded()) {
                AddDebugLog(L"Stream has ended normally");
                break;
            }
            
            // Check if writer is still active
            if (!memory_map.IsWriterActive()) {
                AddDebugLog(L"Writer is no longer active");
                break;
            }
            
            // Check for timeout
            if (consecutive_empty_reads >= MAX_EMPTY_READS) {
                AddDebugLog(L"Timeout waiting for data (no data for 5 seconds)");
                break;
            }
            
            // Wait briefly before next read
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    AddDebugLog(L"Viewer ending, total bytes streamed: " + std::to_wstring(total_bytes_read));
    
    // Cleanup
    memory_map.Close();
    
    return 0;
}