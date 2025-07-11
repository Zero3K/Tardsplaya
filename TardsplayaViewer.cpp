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
    if (argc != 3) {
        std::wcerr << L"Usage: TardsplayaViewer.exe <stream_name> <player_path>" << std::endl;
        std::wcerr << L"This program reads stream data from a memory-mapped file and pipes it to the media player" << std::endl;
        return 1;
    }

    std::wstring stream_name = argv[1];
    std::wstring player_path = argv[2];
    AddDebugLog(L"Starting viewer for stream: " + stream_name + L", player: " + player_path);

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

    // Launch the media player with stdin pipe
    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    
    // Create pipes for stdin
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        AddDebugLog(L"Failed to create pipe for media player");
        return 1;
    }
    
    // Make sure the write handle is not inherited
    SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);
    
    si.hStdInput = hReadPipe;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    std::wstring player_cmd = L"\"" + player_path + L"\" -";
    AddDebugLog(L"Launching media player: " + player_cmd);
    
    if (!CreateProcessW(nullptr, const_cast<LPWSTR>(player_cmd.c_str()), 
                        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        AddDebugLog(L"Failed to launch media player, Error=" + std::to_wstring(GetLastError()));
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return 1;
    }
    
    // Close the read handle in the parent process
    CloseHandle(hReadPipe);
    
    AddDebugLog(L"Media player launched successfully, PID=" + std::to_wstring(pi.dwProcessId));
    
    AddDebugLog(L"Starting data streaming to media player...");
    
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
            // Write data to media player stdin
            DWORD bytes_written;
            
            if (!WriteFile(hWritePipe, buffer.data(), static_cast<DWORD>(bytes_read), &bytes_written, nullptr)) {
                AddDebugLog(L"Failed to write to media player pipe, player may have disconnected");
                break;
            }
            
            if (bytes_written != bytes_read) {
                AddDebugLog(L"Incomplete write to media player: " + std::to_wstring(bytes_written) + 
                           L"/" + std::to_wstring(bytes_read) + L" bytes");
                break;
            }
            
            total_bytes_read += bytes_read;
            consecutive_empty_reads = 0;
            
            // Flush pipe to ensure data is sent immediately
            FlushFileBuffers(hWritePipe);
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
    CloseHandle(hWritePipe);
    
    // Wait for media player to finish
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    return 0;
}