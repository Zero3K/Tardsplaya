#include "stream_pipe.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <chrono>
#include <sstream>
#include <winhttp.h>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include "tlsclient/tlsclient.h"

// Utility: HTTP GET (returns as binary), with error retries
static bool HttpGetBinary(const std::wstring& url, std::vector<char>& out, int max_attempts = 3, std::atomic<bool>* cancel_token = nullptr) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (cancel_token && cancel_token->load()) return false;
        URL_COMPONENTS uc = { sizeof(uc) };
        wchar_t host[256] = L"", path[2048] = L"";
        uc.lpszHostName = host; uc.dwHostNameLength = 255;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
        WinHttpCrackUrl(url.c_str(), 0, 0, &uc);

        HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
        if (!hSession) continue;
        HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); continue; }
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        BOOL res = WinHttpSendRequest(hRequest, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hRequest, 0);
        if (!res) { 
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); 
            std::this_thread::sleep_for(std::chrono::milliseconds(600)); // retry delay
            continue; 
        }

        DWORD dwSize = 0;
        out.clear();
        bool error = false;
        do {
            if (cancel_token && cancel_token->load()) { error = true; break; }
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            size_t prev_size = out.size();
            out.resize(prev_size + dwSize);
            if (!WinHttpReadData(hRequest, out.data() + prev_size, dwSize, &dwDownloaded) || dwDownloaded == 0)
            {
                error = true;
                break;
            }
            if (dwDownloaded < dwSize) out.resize(prev_size + dwDownloaded);
        } while (dwSize > 0);

        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        if (!error && !out.empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(600)); // retry delay
    }
    return false;
}

// Utility: HTTP GET (returns as string)
static bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token = nullptr) {
    std::vector<char> data;
    if (!HttpGetBinary(url, data, 3, cancel_token)) return false;
    out.assign(data.begin(), data.end());
    return true;
}

// Helper: join relative URL to base
static std::wstring JoinUrl(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}

// Parse media segment URLs from m3u8 playlist
static std::vector<std::wstring> ParseSegments(const std::string& playlist, bool* is_ended = nullptr) {
    std::vector<std::wstring> segs;
    std::istringstream ss(playlist);
    std::string line;
    bool ended = false;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            // Check for HLS stream end indicator
            if (line.find("#EXT-X-ENDLIST") == 0) {
                ended = true;
            }
            continue;
        }
        // Should be a .ts or .aac segment
        std::wstring wline(line.begin(), line.end());
        segs.push_back(wline);
    }
    if (is_ended) *is_ended = ended;
    return segs;
}

// Returns true if process handle is still alive - with robust checking for multi-stream scenarios
static bool ProcessStillRunning(HANDLE hProcess) {
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD code = 0;
    if (!GetExitCodeProcess(hProcess, &code)) {
        // GetExitCodeProcess failed - handle might be invalid
        return false;
    }
    
    if (code != STILL_ACTIVE) {
        // Process has actually exited
        return false;
    }
    
    // Additional verification: Use WaitForSingleObject with 0 timeout to check if process is signaled
    // This helps detect processes that are in a zombie state or have crashed but haven't been reaped
    DWORD waitResult = WaitForSingleObject(hProcess, 0);
    if (waitResult == WAIT_OBJECT_0) {
        // Process handle is signaled, meaning the process has terminated
        return false;
    }
    
    // If we reach here, the process is still running
    return true;
}

// Structure for finding windows by process ID
struct FindWindowData {
    DWORD processId;
    HWND hwnd;
};

// Callback function to find window by process ID
static BOOL CALLBACK FindWindowByProcessId(HWND hwnd, LPARAM lParam) {
    FindWindowData* data = reinterpret_cast<FindWindowData*>(lParam);
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    
    if (processId == data->processId && IsWindowVisible(hwnd)) {
        // Check if this is a main window (has a title bar and is not a child window)
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        if ((style & WS_CAPTION) && GetParent(hwnd) == NULL) {
            data->hwnd = hwnd;
            return FALSE; // Stop enumeration
        }
    }
    return TRUE; // Continue enumeration
}

// Function to set and maintain the title of the player window
static void SetPlayerWindowTitle(DWORD processId, const std::wstring& title) {
    if (title.empty()) return;
    
    // Try multiple times as the player window might take time to appear
    HWND playerWindow = nullptr;
    for (int attempts = 0; attempts < 10; ++attempts) {
        FindWindowData data = { processId, NULL };
        EnumWindows(FindWindowByProcessId, reinterpret_cast<LPARAM>(&data));
        
        if (data.hwnd) {
            playerWindow = data.hwnd;
            break;
        }
        
        // Wait before trying again
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    if (!playerWindow) return;
    
    // Set the title initially and then monitor it to keep it set
    SetWindowTextW(playerWindow, title.c_str());
    
    // Continue monitoring the title and reset it if it changes
    // This handles cases where the player might reset the title to "stdin"
    for (int monitor_attempts = 0; monitor_attempts < 60; ++monitor_attempts) {  // Monitor for 30 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Check if the window still exists
        if (!IsWindow(playerWindow)) break;
        
        // Check current title
        wchar_t currentTitle[256];
        int titleLen = GetWindowTextW(playerWindow, currentTitle, 256);
        
        // If title is different from what we want, reset it
        if (titleLen == 0 || wcscmp(currentTitle, title.c_str()) != 0) {
            SetWindowTextW(playerWindow, title.c_str());
        }
    }
}

bool BufferAndPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
) {
    // Seed random number generator for network request staggering
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(nullptr));
        seeded = true;
    }
    
    // 1. Download the master playlist and pick the first media playlist (or use playlist_url directly if it's media)
    std::string master;
    if (cancel_token.load()) return false;
    if (!HttpGetText(playlist_url, master, &cancel_token)) return false;

    // If this is a master playlist, pick the first stream URL
    std::wstring media_playlist_url = playlist_url;
    bool is_master = false;
    std::istringstream ss(master);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("#EXT-X-STREAM-INF:") == 0) is_master = true;
        if (is_master && !line.empty() && line[0] != '#') {
            media_playlist_url = JoinUrl(playlist_url, std::wstring(line.begin(), line.end()));
            break;
        }
    }

    // 2. Create a pipe for stdin redirection to the player with larger buffer for multiple streams
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    // Use larger pipe buffer when multiple streams might be running
    // This helps prevent pipe buffer overflows that can cause player crashes
    DWORD pipeBufferSize = 65536; // 64KB buffer per stream
    if (!CreatePipe(&hRead, &hWrite, &sa, pipeBufferSize)) return false;

    // Create separate output/error handles for process isolation to prevent conflicts between multiple streams
    HANDLE hStdOut = NULL, hStdErr = NULL;
    HANDLE hNullOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE hNullErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    
    // Use separate null handles for output/error to prevent interference between multiple players
    hStdOut = (hNullOut != INVALID_HANDLE_VALUE) ? hNullOut : GetStdHandle(STD_OUTPUT_HANDLE);
    hStdErr = (hNullErr != INVALID_HANDLE_VALUE) ? hNullErr : GetStdHandle(STD_ERROR_HANDLE);

    // 3. Launch the player with stdin redirected and isolated output/error handles
    std::wstring cmd = L"\"" + player_path + L"\" -";
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = hStdOut;
    si.hStdError = hStdErr;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        nullptr, (LPWSTR)cmd.c_str(),
        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi
    );
    CloseHandle(hRead);
    if (!ok) { 
        CloseHandle(hWrite); 
        if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
        if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
        return false; 
    }

    // Set the player window title to the channel name (in a separate thread to avoid blocking)
    if (!channel_name.empty()) {
        std::thread([processId = pi.dwProcessId, title = channel_name]() {
            SetPlayerWindowTitle(processId, title);
        }).detach();
    }

    // 4. Download media playlist and buffer segments
    std::set<std::wstring> downloaded;
    std::vector<std::vector<char>> segment_buffer;
    bool started = false;
    int consecutive_errors = 0;
    const int max_consecutive_errors = 30; // ~60 seconds with 2s sleep - more tolerant for live streams
    bool stream_ended = false;

    // Add small random delay for multiple streams to spread out network requests
    std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 500));

    // Track consecutive process checks to avoid false positives under resource pressure
    int consecutive_process_failures = 0;
    const int max_process_failures = 3; // Allow 3 consecutive failures before giving up
    
    // Detect if multiple streams might be running and adjust polling frequency
    // Use a longer random delay to spread out network requests more when system is under load
    int base_poll_interval = 2000; // 2 seconds base
    int random_jitter = rand() % 1000; // 0-1000ms additional jitter
    int poll_interval = base_poll_interval + random_jitter;
    
    while (!cancel_token.load() && consecutive_process_failures < max_process_failures && !stream_ended) {
        // Check if process is still running, but allow for temporary suspension/resource pressure
        if (!ProcessStillRunning(pi.hProcess)) {
            consecutive_process_failures++;
            if (consecutive_process_failures >= max_process_failures) {
                // Process appears to be genuinely dead after multiple checks
                break;
            }
            // Process might be temporarily suspended - wait and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        } else {
            // Process is running - reset failure counter
            consecutive_process_failures = 0;
        }
        std::string playlist;
        if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
            consecutive_errors++;
            if (consecutive_errors > max_consecutive_errors) {
                // Too many consecutive errors - likely stream is down or network issues
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        consecutive_errors = 0;
        
        // Parse segments and check for stream end
        bool playlist_ended = false;
        auto segments = ParseSegments(playlist, &playlist_ended);
        if (playlist_ended) {
            stream_ended = true;
        }

        size_t new_segments = 0;
        for (auto& seg : segments) {
            if (cancel_token.load()) break;
            // Use less aggressive process checking in the inner loop
            if (consecutive_process_failures >= max_process_failures) break;
            if (downloaded.count(seg)) continue;
            downloaded.insert(seg);

            // Download segment (with retry)
            std::wstring seg_url = JoinUrl(media_playlist_url, seg);
            std::vector<char> segment_data;
            int seg_attempts = 0;
            while (seg_attempts < 3 && !HttpGetBinary(seg_url, segment_data, 1, &cancel_token)) {
                if (cancel_token.load()) break;
                // Only check process health on segment download failures to reduce overhead
                if (seg_attempts > 0 && !ProcessStillRunning(pi.hProcess)) {
                    consecutive_process_failures++;
                    if (consecutive_process_failures >= max_process_failures) break;
                }
                seg_attempts++;
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
            if (consecutive_process_failures >= max_process_failures) break;
            if (!segment_data.empty()) {
                segment_buffer.push_back(std::move(segment_data));
                new_segments++;
                
                // Update chunk count for status display
                if (chunk_count) {
                    *chunk_count = (int)segment_buffer.size();
                }

                // Once buffer is filled, start writing to player
                if (!started && (int)segment_buffer.size() >= buffer_segments) {
                    started = true;
                    for (auto& buf : segment_buffer) {
                        if (cancel_token.load()) break;
                        // Check process health before writing each buffer segment
                        if (!ProcessStillRunning(pi.hProcess)) {
                            consecutive_process_failures++;
                            if (consecutive_process_failures >= max_process_failures) break;
                            // Process might be temporarily suspended - wait briefly and continue
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            continue;
                        }
                        DWORD written = 0;
                        // Check for write errors that could indicate player crash/exit
                        if (!WriteFile(hWrite, buf.data(), (DWORD)buf.size(), &written, nullptr)) {
                            // Pipe broken - player likely crashed or exited unexpectedly
                            // Exit the streaming loop to avoid further errors
                            consecutive_process_failures = max_process_failures; // Force exit
                            goto streaming_cleanup;
                        }
                        // Verify we wrote the expected amount
                        if (written != buf.size()) {
                            // Partial write - pipe may be full or player struggling
                            // This could indicate resource contention with multiple streams
                            break;
                        }
                        // Small delay between segments when writing initial buffer
                        // This helps prevent overwhelming the player with multiple concurrent streams
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if (consecutive_process_failures >= max_process_failures) break;
                    segment_buffer.clear();
                    // Update chunk count after clearing buffer
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                } else if (started) {
                    if (cancel_token.load()) break;
                    // Check process health before writing
                    if (!ProcessStillRunning(pi.hProcess)) {
                        consecutive_process_failures++;
                        if (consecutive_process_failures >= max_process_failures) break;
                        // Process might be temporarily suspended - wait briefly and continue
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    auto& buf = segment_buffer.front();
                    DWORD written = 0;
                    // Check for write errors that could indicate player crash/exit
                    if (!WriteFile(hWrite, buf.data(), (DWORD)buf.size(), &written, nullptr)) {
                        // Pipe broken - player likely crashed or exited unexpectedly
                        // Exit the streaming loop to avoid further errors
                        consecutive_process_failures = max_process_failures; // Force exit
                        goto streaming_cleanup;
                    }
                    // Verify we wrote the expected amount
                    if (written != buf.size()) {
                        // Partial write - pipe may be full or player struggling
                        // This could indicate resource contention with multiple streams
                        // Continue but don't remove the segment so it can be retried
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    segment_buffer.erase(segment_buffer.begin());
                    // Update chunk count after removing segment
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                }
            }
        }

        if (cancel_token.load() || consecutive_process_failures >= max_process_failures) break;
        if (segments.empty() && !stream_ended) { // No segments but stream hasn't ended - continue polling
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval));
            continue;
        }

        // Wait before polling playlist again - use adaptive interval to reduce load
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval));
    }

streaming_cleanup:
    // 5. Cleanup: close write pipe, let player exit
    CloseHandle(hWrite);
    if (ProcessStillRunning(pi.hProcess)) {
        // Ask player to exit
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    
    // Cleanup the isolated output/error handles if they were created
    if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
    if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
    
    // Return true only if stream ended normally (with #EXT-X-ENDLIST)
    // Return false for all other cases (network errors, player crashes, user cancellation)
    // User cancellation is handled separately via user_requested_stop flag
    // Also return false if we hit max process failures (likely player crash/resource issues)
    return stream_ended && consecutive_process_failures < max_process_failures;
}