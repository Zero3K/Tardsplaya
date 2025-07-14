#include "stream_pipe.h"
#include "stream_thread.h"
#include "twitch_api.h"
#include "playlist_parser.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <chrono>
#include <sstream>
#include <winhttp.h>
#include <atomic>
#include <iostream>
#include <queue>
#include <mutex>
#include "tlsclient/tlsclient.h"

// Forward declarations for functions defined in Tardsplaya.cpp
std::wstring GetAccessToken(const std::wstring& channel);
std::wstring FetchPlaylist(const std::wstring& channel, const std::wstring& accessToken);
std::map<std::wstring, std::wstring> ParsePlaylist(const std::wstring& m3u8);

// Forward declarations for functions defined in other files
bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token = nullptr);
std::wstring Utf8ToWide(const std::string& str);

// Helper functions for URL parsing
std::wstring ExtractDomain(const std::wstring& url) {
    size_t start = url.find(L"://");
    if (start == std::wstring::npos) return url;
    start += 3;
    size_t end = url.find(L'/', start);
    if (end == std::wstring::npos) return url.substr(start);
    return url.substr(start, end - start);
}

std::wstring ExtractPath(const std::wstring& url) {
    size_t start = url.find(L"://");
    if (start == std::wstring::npos) return L"/";
    start = url.find(L'/', start + 3);
    if (start == std::wstring::npos) return L"/";
    return url.substr(start);
}

// Global stream tracking for multi-stream debugging
static std::atomic<int> g_active_streams(0);
static std::mutex g_stream_mutex;

// Robust HTTP server for localhost streaming with persistent buffering
class StreamHttpServer {
private:
    SOCKET listen_socket;
    int port;
    std::atomic<bool> running;
    std::queue<std::vector<char>> data_queue;
    mutable std::mutex queue_mutex;
    std::atomic<bool> stream_ended;
    std::thread server_thread;
    std::atomic<size_t> buffer_size;
    std::atomic<int> active_connections;
    static const int MAX_CONNECTIONS = 2; // Limit concurrent connections per stream
    
public:
    StreamHttpServer() : listen_socket(INVALID_SOCKET), port(0), running(false), stream_ended(false), buffer_size(0), active_connections(0) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }
    
    ~StreamHttpServer() {
        Stop();
        WSACleanup();
    }
    
    bool Start(int preferred_port = 8080) {
        // Find available port starting from preferred_port
        for (int try_port = preferred_port; try_port < preferred_port + 100; try_port++) {
            listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listen_socket == INVALID_SOCKET) continue;
            
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(try_port);
            
            if (bind(listen_socket, (sockaddr*)&addr, sizeof(addr)) == 0) {
                if (listen(listen_socket, 5) == 0) {
                    port = try_port;
                    running = true;
                    
                    // Start server thread
                    server_thread = std::thread([this]() { ServerLoop(); });
                    return true;
                }
            }
            closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
        return false;
    }
    
    void Stop() {
        running = false;
        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
    
    int GetPort() const { return port; }
    
    void AddData(const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        data_queue.push(data);
        buffer_size += data.size();
        // Maintain minimum queue size to prevent freezing
        // AddDebugLog for excessive logging can be enabled if needed
        // AddDebugLog(L"[HTTP] Added " + std::to_wstring(data.size()) + L" bytes, queue=" + std::to_wstring(data_queue.size()));
    }
    
    size_t GetMinQueueSize() const {
        return 3; // Minimum 3 segments to prevent video freezing
    }
    
    size_t GetBufferSize() const {
        return buffer_size.load();
    }
    
    size_t GetQueueLength() const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return data_queue.size();
    }
    
    void SetStreamEnded() {
        stream_ended = true;
    }
    
private:
    void ServerLoop() {
        while (running) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_socket, &read_fds);
            
            timeval timeout = { 1, 0 }; // 1 second timeout
            int result = select(0, &read_fds, nullptr, nullptr, &timeout);
            
            if (result > 0 && FD_ISSET(listen_socket, &read_fds)) {
                // Limit concurrent connections to prevent resource exhaustion
                if (active_connections.load() < MAX_CONNECTIONS) {
                    SOCKET client_socket = accept(listen_socket, nullptr, nullptr);
                    if (client_socket != INVALID_SOCKET) {
                        active_connections++;
                        std::thread([this, client_socket]() { 
                            HandleClient(client_socket); 
                            active_connections--;
                        }).detach();
                    }
                } else {
                    // Too many connections, reject new ones to prevent resource exhaustion
                    SOCKET client_socket = accept(listen_socket, nullptr, nullptr);
                    if (client_socket != INVALID_SOCKET) {
                        closesocket(client_socket);
                    }
                }
            }
        }
    }
    
    void HandleClient(SOCKET client_socket) {
        // Read HTTP request (we don't really need to parse it)
        char buffer[1024];
        recv(client_socket, buffer, sizeof(buffer), 0);
        
        // Send HTTP response headers
        std::string response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/mp2t\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(client_socket, response.c_str(), (int)response.length(), 0);
        
        // Stream data from queue
        int segments_sent = 0;
        int empty_queue_count = 0;
        const int max_empty_waits = 100; // 5 seconds max wait for data (50ms * 100)
        
        while (running && !stream_ended) {
            std::vector<char> data;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!data_queue.empty()) {
                    data = std::move(data_queue.front());
                    data_queue.pop();
                    buffer_size -= data.size();
                    empty_queue_count = 0; // Reset counter when data is available
                }
            }
            
            if (!data.empty()) {
                // Send data with retry logic to handle slow writes and write failures
                bool data_sent = false;
                int send_attempts = 0;
                const int max_send_attempts = 3;
                const char* data_ptr = data.data();
                int remaining_bytes = (int)data.size();
                
                while (!data_sent && send_attempts < max_send_attempts && remaining_bytes > 0) {
                    int sent = send(client_socket, data_ptr, remaining_bytes, 0);
                    
                    if (sent > 0) {
                        data_ptr += sent;
                        remaining_bytes -= sent;
                        
                        if (remaining_bytes == 0) {
                            data_sent = true;
                        }
                        // Continue sending remaining bytes if partial send occurred
                    } else {
                        send_attempts++;
                        int error = WSAGetLastError();
                        
                        if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN) {
                            AddDebugLog(L"[HTTP] Client disconnected (error=" + std::to_wstring(error) + L") after " + 
                                       std::to_wstring(segments_sent) + L" segments");
                            break; // Client disconnected, no point retrying
                        }
                        
                        if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
                            AddDebugLog(L"[HTTP] Socket send would block, attempt " + std::to_wstring(send_attempts) + 
                                       L"/" + std::to_wstring(max_send_attempts));
                            if (send_attempts < max_send_attempts) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Wait for socket to become ready
                            }
                        } else {
                            AddDebugLog(L"[HTTP] Send error " + std::to_wstring(error) + L", attempt " + 
                                       std::to_wstring(send_attempts) + L"/" + std::to_wstring(max_send_attempts));
                            if (send_attempts < max_send_attempts) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }
                    }
                }
                
                if (!data_sent) {
                    AddDebugLog(L"[HTTP] Failed to send data after " + std::to_wstring(max_send_attempts) + 
                               L" attempts, client likely disconnected");
                    break;
                }
                segments_sent++;
            } else {
                // No data available - wait for more data
                empty_queue_count++;
                if (empty_queue_count >= max_empty_waits) {
                    AddDebugLog(L"[HTTP] No data for too long, ending client session");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        
        AddDebugLog(L"[HTTP] Client session ended, sent " + std::to_wstring(segments_sent) + L" segments");
        closesocket(client_socket);
    }
};

/*
 * WRITE FAILURE PREVENTION MECHANISMS:
 * 
 * This implementation focuses on preventing slow writes and write failures instead of 
 * reducing timing delays to prevent video freezing:
 *
 * 1. WriteFile Retry Logic: Each write operation attempts up to 3 times with error recovery
 * 2. Timeout Handling: WriteFileWithTimeout prevents indefinite blocking on slow writes  
 * 3. Partial Write Handling: Socket sends handle partial writes by continuing from offset
 * 4. Error-Specific Recovery: Different error types (broken pipe, timeout, etc.) handled appropriately
 * 5. Buffered Pipes: Larger pipe buffer (1MB) reduces chance of write blocking
 * 6. Resource Monitoring: Track write attempts and failures for diagnostics
 *
 * Previous approach reduced delays from 50ms to 10ms to prevent freezing, but this 
 * approach maintains 50ms delays and focuses on robust write operations instead.
 */

// Helper function to perform WriteFile with timeout for slow write prevention
static bool WriteFileWithTimeout(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite, 
                                DWORD* lpNumberOfBytesWritten, DWORD timeoutMs = 5000) {
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        return FALSE;
    }
    
    BOOL result = WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, &overlapped);
    
    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // Wait for the write to complete with timeout
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
            if (waitResult == WAIT_OBJECT_0) {
                result = GetOverlappedResult(hFile, &overlapped, lpNumberOfBytesWritten, FALSE);
            } else if (waitResult == WAIT_TIMEOUT) {
                // Cancel the I/O operation if it times out
                CancelIo(hFile);
                SetLastError(ERROR_TIMEOUT);
                result = FALSE;
            } else {
                result = FALSE;
            }
        } else {
            result = FALSE;
        }
    }
    
    CloseHandle(overlapped.hEvent);
    return result;
}

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
static bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token) {
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

// Parse media segment URLs from m3u8 playlist, filtering out ad segments
// Based on Twitch HLS AdBlock extension logic
static std::vector<std::wstring> ParseSegments(const std::string& playlist) {
    std::vector<std::wstring> segs;
    std::istringstream ss(playlist);
    std::string line;
    bool in_scte35_out = false;
    bool skip_next_segment = false;
    
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // SCTE-35 ad marker detection (start of ad block)
            if (line.find("#EXT-X-SCTE35-OUT") == 0) {
                in_scte35_out = true;
                skip_next_segment = true;
                AddDebugLog(L"[FILTER] Found SCTE35-OUT marker, entering ad block");
                continue;
            }
            // SCTE-35 ad marker detection (end of ad block)
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                in_scte35_out = false;
                AddDebugLog(L"[FILTER] Found SCTE35-IN marker, exiting ad block");
                continue;
            }
            // Discontinuity markers (often used with ads)
            else if (line.find("#EXT-X-DISCONTINUITY") == 0 && in_scte35_out) {
                AddDebugLog(L"[FILTER] Skipping discontinuity marker in ad block");
                continue;
            }
            // Stitched ad segments
            else if (line.find("stitched-ad") != std::string::npos) {
                skip_next_segment = true;
                AddDebugLog(L"[FILTER] Found stitched-ad marker");
            }
            // EXTINF tags with specific durations that indicate ads (2.001, 2.002 seconds)
            else if (line.find("#EXTINF:2.00") == 0 && 
                     (line.find("2.001") != std::string::npos || 
                      line.find("2.002") != std::string::npos)) {
                skip_next_segment = true;
                AddDebugLog(L"[FILTER] Found ad-duration EXTINF marker");
            }
            // DATERANGE markers for stitched ads
            else if (line.find("#EXT-X-DATERANGE:ID=\"stitched-ad") == 0) {
                skip_next_segment = true;
                AddDebugLog(L"[FILTER] Found stitched-ad DATERANGE marker");
            }
            // General stitched content detection
            else if (line.find("stitched") != std::string::npos ||
                     line.find("STITCHED") != std::string::npos) {
                skip_next_segment = true;
                AddDebugLog(L"[FILTER] Found general stitched content marker");
            }
            // MIDROLL ad markers
            else if (line.find("EXT-X-DATERANGE") != std::string::npos && 
                     (line.find("MIDROLL") != std::string::npos ||
                      line.find("midroll") != std::string::npos)) {
                skip_next_segment = true;
                AddDebugLog(L"[FILTER] Found MIDROLL ad marker");
            }
            continue;
        }
        
        // This is a segment URL
        if (skip_next_segment || in_scte35_out) {
            // Ad segment detected - trigger fresh playlist fetch instead of skipping
            AddDebugLog(L"[FILTER] Ad segment detected: " + std::wstring(line.begin(), line.end()));
            AddDebugLog(L"[FILTER] Triggering fresh playlist fetch to bypass ads");
            
            // Add a special marker to signal fresh playlist fetch is needed
            segs.push_back(L"__FETCH_FRESH_PLAYLIST__");
            
            skip_next_segment = false;
            continue;
        }
        
        // Should be a .ts or .aac segment
        std::wstring wline(line.begin(), line.end());
        segs.push_back(wline);
    }
    return segs;
}

// Returns true if process handle is still alive
static bool ProcessStillRunning(HANDLE hProcess, const std::wstring& debug_context = L"", DWORD pid = 0) {
    if (hProcess == INVALID_HANDLE_VALUE || hProcess == nullptr) {
        if (!debug_context.empty()) {
            AddDebugLog(L"[PROCESS] Invalid handle for " + debug_context);
        }
        return false;
    }
    
    DWORD code = 0;
    BOOL result = GetExitCodeProcess(hProcess, &code);
    bool still_active = result && (code == STILL_ACTIVE);
    
    // Additional detailed logging for process state
    if (!debug_context.empty()) {
        DWORD wait_result = WaitForSingleObject(hProcess, 0);
        bool wait_timeout = (wait_result == WAIT_TIMEOUT);
        
        AddDebugLog(L"[PROCESS] Detailed check for " + debug_context + 
                   L": GetExitCodeProcess=" + std::to_wstring(result) +
                   L", ExitCode=" + std::to_wstring(code) + 
                   L", STILL_ACTIVE=" + std::to_wstring(STILL_ACTIVE) +
                   L", WaitResult=" + std::to_wstring(wait_result) +
                   L", WaitTimeout=" + std::to_wstring(wait_timeout) +
                   L", LastError=" + std::to_wstring(GetLastError()));
    }
    
    // Double-check using PID if we have it
    if (!still_active && pid != 0) {
        HANDLE pidHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (pidHandle != nullptr) {
            DWORD pidCode = 0;
            BOOL pidResult = GetExitCodeProcess(pidHandle, &pidCode);
            bool pidActive = pidResult && (pidCode == STILL_ACTIVE);
            CloseHandle(pidHandle);
            
            if (pidActive && !still_active) {
                AddDebugLog(L"[PROCESS] Handle check failed but PID check succeeded for " + debug_context +
                           L", may be handle corruption - using PID result");
                return true; // Trust PID check over handle check
            }
            
            AddDebugLog(L"[PROCESS] PID verification for " + debug_context + 
                       L": PIDResult=" + std::to_wstring(pidResult) +
                       L", PIDCode=" + std::to_wstring(pidCode) + 
                       L", PIDActive=" + std::to_wstring(pidActive));
        } else {
            AddDebugLog(L"[PROCESS] Could not open PID " + std::to_wstring(pid) + 
                       L" for verification, Error=" + std::to_wstring(GetLastError()));
        }
    }
    
    // Log final result
    if (!debug_context.empty()) {
        if (still_active) {
            AddDebugLog(L"[PROCESS] Process ALIVE for " + debug_context);
        } else {
            AddDebugLog(L"[PROCESS] Process DEAD for " + debug_context + 
                       L" (ExitCode=" + std::to_wstring(code) + L")");
        }
    }
    
    return still_active;
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
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    // Track active streams for cross-stream interference detection
    int current_stream_count;
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        current_stream_count = ++g_active_streams;
    }
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting IPC streaming for " + channel_name + L", URL=" + playlist_url);
    AddDebugLog(L"[STREAMS] This is stream #" + std::to_wstring(current_stream_count) + L" concurrently active");
    
    // Add startup delay for multi-stream scenarios to reduce resource contention
    if (current_stream_count > 1) {
        int delay_ms = (current_stream_count - 1) * 500; // 500ms per additional stream
        AddDebugLog(L"[STREAMS] Adding " + std::to_wstring(delay_ms) + L"ms startup delay for stream " + 
                   std::to_wstring(current_stream_count) + L" (" + channel_name + L")");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    
    // Log system resource state before starting new stream
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    DWORD processCount = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(snapshot, &pe32)) {
            do {
                processCount++;
            } while (Process32Next(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }
    
    AddDebugLog(L"[RESOURCE] System state before stream start: MemoryLoad=" + std::to_wstring(memInfo.dwMemoryLoad) + 
               L"%, AvailPhysMB=" + std::to_wstring(memInfo.ullAvailPhys / (1024*1024)) + 
               L", ProcessCount=" + std::to_wstring(processCount) + L" for " + channel_name);
    
    // 1. Download the master playlist and pick the first media playlist
    std::string master;
    if (cancel_token.load()) return false;
    if (!HttpGetText(playlist_url, master, &cancel_token)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to download master playlist for " + channel_name);
        // Decrement stream counter on early exit
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        --g_active_streams;
        return false;
    }

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
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Using media playlist URL=" + media_playlist_url + L" for " + channel_name);

    // 2. Create media player process with stdin piping for IPC
    
    // Create pipe for stdin communication
    HANDLE hStdinRead, hStdinWrite;
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
    
    // Create pipe with larger buffer size to help prevent slow writes
    const DWORD PIPE_BUFFER_SIZE = 1024 * 1024; // 1MB buffer to handle slow writes
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &saAttr, PIPE_BUFFER_SIZE)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create pipe for " + channel_name);
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        --g_active_streams;
        return false;
    }
    
    // Ensure the write handle is not inherited
    if (!SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to set handle information for " + channel_name);
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        --g_active_streams;
        return false;
    }
    
    // Build command with media player configured to read from stdin
    std::wstring cmd;
    if (player_path.find(L"mpc-hc") != std::wstring::npos) {
        // MPC-HC: read from stdin
        cmd = L"\"" + player_path + L"\" - /new /nofocus";
    } else if (player_path.find(L"vlc") != std::wstring::npos) {
        // VLC: read from stdin
        cmd = L"\"" + player_path + L"\" - --intf dummy --no-one-instance";
    } else {
        // Generic media player: read from stdin
        cmd = L"\"" + player_path + L"\" -";
    }
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = hStdinRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Launching IPC player: " + cmd + L" for " + channel_name);
    
    // Record timing for process creation
    auto start_time = std::chrono::high_resolution_clock::now();
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi
    );
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Close the read handle since the child process now owns it
    CloseHandle(hStdinRead);

    if (!success) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create process for " + channel_name + 
                   L", Error=" + std::to_wstring(error) + L", Duration=" + std::to_wstring(duration.count()) + L"ms");
        // Clean up pipe handle
        CloseHandle(hStdinWrite);
        // Decrement stream counter on early exit
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        --g_active_streams;
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Process created successfully for " + channel_name + 
               L", PID=" + std::to_wstring(pi.dwProcessId) + L", Duration=" + std::to_wstring(duration.count()) + L"ms");
    
    // Set process priority for better multi-stream performance
    int stream_count = g_active_streams.load(); // Update count before setting priority
    if (stream_count > 1) {
        // Lower priority for additional streams to reduce conflicts
        SetPriorityClass(pi.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
        AddDebugLog(L"[IPC] Set BELOW_NORMAL priority for stream #" + std::to_wstring(stream_count) + L" (" + channel_name + L")");
    } else {
        // Normal priority for first stream
        SetPriorityClass(pi.hProcess, NORMAL_PRIORITY_CLASS);
        AddDebugLog(L"[IPC] Set NORMAL priority for first stream (" + channel_name + L")");
    }
    
    // Verify process is actually running immediately after creation
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Brief delay for process startup
    bool initial_check = ProcessStillRunning(pi.hProcess, channel_name + L" initial_verification", pi.dwProcessId);
    AddDebugLog(L"[PROCESS] Initial verification after 100ms: " + std::to_wstring(initial_check) + L" for " + channel_name);

    // 3. Start thread to maintain player window title with channel name
    std::thread title_thread([pi, channel_name]() {
        SetPlayerWindowTitle(pi.dwProcessId, channel_name);
    });
    title_thread.detach();

    // 4. IPC streaming with background download threads and direct piping
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::set<std::wstring> seen_urls;
    std::atomic<bool> download_running(true);
    std::atomic<bool> stream_ended_normally(false);
    std::atomic<bool> urgent_download_needed(false); // Signal for immediate download when buffer reaches 0
    
    // Store pipe handle for cleanup
    HANDLE stdin_pipe = hStdinWrite;
    
    const int target_buffer_segments = std::max(buffer_segments, 10); // Minimum 10 segments
    const int max_buffer_segments = target_buffer_segments * 2; // Don't over-buffer
    const int buffer_full_timeout_seconds = 15; // Empty buffer if full for this long
    
    // Buffer fullness timeout tracking
    std::chrono::steady_clock::time_point buffer_full_start_time;
    bool buffer_full_timer_active = false;
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Target buffer: " + std::to_wstring(target_buffer_segments) + 
               L" segments, max: " + std::to_wstring(max_buffer_segments) + 
               L", timeout: " + std::to_wstring(buffer_full_timeout_seconds) + L"s for " + channel_name);

    // Log thread creation timing
    auto thread_start_time = std::chrono::high_resolution_clock::now();
    AddDebugLog(L"[THREAD] Creating download and feeder threads for " + channel_name);

    // Background playlist monitor and segment downloader thread
    std::thread download_thread([&]() {
        auto thread_actual_start = std::chrono::high_resolution_clock::now();
        auto startup_delay = std::chrono::duration_cast<std::chrono::milliseconds>(thread_actual_start - thread_start_time);
        
        int consecutive_errors = 0;
        const int max_consecutive_errors = 15; // ~30 seconds (2 sec intervals)
        
        int fresh_playlist_attempts = 0;
        const int max_fresh_playlist_attempts = 5; // Limit fresh playlist fetches to prevent infinite loops
        auto last_fresh_playlist_time = std::chrono::steady_clock::now();
        const std::chrono::seconds fresh_playlist_cooldown(30); // 30 second cooldown between attempts
        
        AddDebugLog(L"[DOWNLOAD] Starting download thread for " + channel_name + 
                   L", startup_delay=" + std::to_wstring(startup_delay.count()) + L"ms");
        
        while (true) {
            // Check for urgent download request first
            bool urgent_needed = urgent_download_needed.load();
            if (urgent_needed) {
                AddDebugLog(L"[DOWNLOAD] *** URGENT DOWNLOAD REQUESTED *** - buffer reached 0 for " + channel_name);
            }
            
            // Check all exit conditions individually for detailed logging
            bool download_running_check = download_running.load();
            bool cancel_token_check = cancel_token.load();
            bool process_running_check = ProcessStillRunning(pi.hProcess, channel_name + L" download_thread", pi.dwProcessId);
            bool error_limit_check = consecutive_errors < max_consecutive_errors;
            
            if (!download_running_check) {
                AddDebugLog(L"[DOWNLOAD] Exit condition: download_running=false for " + channel_name);
                break;
            }
            if (cancel_token_check) {
                AddDebugLog(L"[DOWNLOAD] Exit condition: cancel_token=true for " + channel_name);
                break;
            }
            if (!process_running_check) {
                AddDebugLog(L"[DOWNLOAD] Exit condition: process died for " + channel_name);
                break;
            }
            if (!error_limit_check) {
                AddDebugLog(L"[DOWNLOAD] Exit condition: too many consecutive errors (" + 
                           std::to_wstring(consecutive_errors) + L") for " + channel_name);
                break;
            }
               
            AddDebugLog(L"[DOWNLOAD] Loop iteration for " + channel_name + 
                       L", consecutive_errors=" + std::to_wstring(consecutive_errors));
            
            // Download current playlist
            std::string playlist;
            AddDebugLog(L"[DOWNLOAD] Fetching playlist for " + channel_name);
            if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
                consecutive_errors++;
                AddDebugLog(L"[DOWNLOAD] Playlist fetch FAILED for " + channel_name + 
                           L", error " + std::to_wstring(consecutive_errors) + L"/" + 
                           std::to_wstring(max_consecutive_errors));
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            consecutive_errors = 0;
            AddDebugLog(L"[DOWNLOAD] Playlist fetch SUCCESS for " + channel_name + 
                       L", size=" + std::to_wstring(playlist.size()) + L" bytes");

            // Check for stream end
            if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
                AddDebugLog(L"[DOWNLOAD] Found #EXT-X-ENDLIST - stream actually ended for " + channel_name);
                stream_ended_normally = true;
                break;
            }

            auto segments = ParseSegments(playlist);
            
            // Count buffer reset markers for monitoring ad filtering effectiveness
            int ad_reset_count = 0;
            for (const auto& seg : segments) {
                if (seg == L"__AD_RESET_BUFFER__") {
                    ad_reset_count++;
                }
            }
            
            AddDebugLog(L"[DOWNLOAD] Parsed " + std::to_wstring(segments.size()) + 
                       L" segments from playlist for " + channel_name + 
                       (ad_reset_count > 0 ? L" (including " + std::to_wstring(ad_reset_count) + L" ad resets)" : L""));
            
            // Download new segments
            int new_segments_downloaded = 0;
            for (auto& seg : segments) {
                if (!download_running || cancel_token.load()) {
                    AddDebugLog(L"[DOWNLOAD] Breaking segment loop - download_running=" + 
                               std::to_wstring(download_running.load()) + L", cancel=" + 
                               std::to_wstring(cancel_token.load()) + L" for " + channel_name);
                    break;
                }
                
                bool process_still_running = ProcessStillRunning(pi.hProcess, channel_name + L" segment_download", pi.dwProcessId);
                if (!process_still_running) {
                    AddDebugLog(L"[DOWNLOAD] Breaking segment loop - media player process died for " + channel_name);
                    break;
                }
                
                // Handle ad segment detection - fetch fresh playlist to bypass ads
                if (seg == L"__FETCH_FRESH_PLAYLIST__") {
                    // Don't add to seen_urls yet - process this marker every time
                    AddDebugLog(L"[AD_RECOVERY] Fresh playlist fetch requested - bypassing ads for " + channel_name);
                    
                    // Check fresh playlist fetch limits and cooldown
                    auto current_time = std::chrono::steady_clock::now();
                    auto time_since_last_fetch = current_time - last_fresh_playlist_time;
                    bool cooldown_passed = time_since_last_fetch >= fresh_playlist_cooldown;
                    bool attempts_remaining = fresh_playlist_attempts < max_fresh_playlist_attempts;
                    
                    if (!attempts_remaining) {
                        AddDebugLog(L"[AD_RECOVERY] Maximum fresh playlist attempts reached (" + 
                                   std::to_wstring(fresh_playlist_attempts) + L"), skipping for " + channel_name);
                        continue;
                    }
                    
                    if (!cooldown_passed) {
                        auto remaining_cooldown = std::chrono::duration_cast<std::chrono::seconds>(fresh_playlist_cooldown - time_since_last_fetch);
                        AddDebugLog(L"[AD_RECOVERY] Fresh playlist cooldown active, " + 
                                   std::to_wstring(remaining_cooldown.count()) + L" seconds remaining for " + channel_name);
                        continue;
                    }
                    
                    AddDebugLog(L"[AD_RECOVERY] Proceeding with fresh playlist fetch (attempt " + 
                               std::to_wstring(fresh_playlist_attempts + 1) + L"/" + 
                               std::to_wstring(max_fresh_playlist_attempts) + L") for " + channel_name);
                    
                    // Implement user's suggestion: fetch fresh quality list and switch to clean playlist
                    if (!channel_name.empty() && !selected_quality.empty()) {
                        // Use the same authentication approach as the main application
                        AddDebugLog(L"[AD_RECOVERY] Getting fresh access token for " + channel_name);
                        std::wstring fresh_access_token = GetAccessToken(channel_name);
                        
                        if (!fresh_access_token.empty() && fresh_access_token != L"OFFLINE") {
                            AddDebugLog(L"[AD_RECOVERY] Successfully obtained fresh access token for " + channel_name);
                            
                            // Fetch fresh master playlist using the authenticated token
                            std::wstring fresh_master_playlist = FetchPlaylist(channel_name, fresh_access_token);
                            
                            if (!fresh_master_playlist.empty()) {
                                AddDebugLog(L"[AD_RECOVERY] Successfully fetched fresh master playlist for " + channel_name);
                                AddDebugLog(L"[AD_RECOVERY] Fresh master playlist content (first 500 chars): " + fresh_master_playlist.substr(0, 500));
                                
                                // Parse the fresh playlist to get quality URLs
                                std::map<std::wstring, std::wstring> fresh_qualities = ParsePlaylist(fresh_master_playlist);
                                
                                if (!fresh_qualities.empty()) {
                                    AddDebugLog(L"[AD_RECOVERY] Successfully parsed " + std::to_wstring(fresh_qualities.size()) + L" qualities from fresh playlist for " + channel_name);
                                    
                                    // Log all available qualities for debugging
                                    std::wstring qualities_list = L"[AD_RECOVERY] Available qualities: ";
                                    for (const auto& quality_pair : fresh_qualities) {
                                        qualities_list += quality_pair.first + L", ";
                                    }
                                    AddDebugLog(qualities_list);
                                    AddDebugLog(L"[AD_RECOVERY] Originally selected quality: " + selected_quality);
                                    
                                    // Find the URL for the user's selected quality
                                    std::wstring fresh_playlist_url;
                                    bool quality_found = false;
                                    std::wstring selected_quality_name;
                                    
                                    // First try exact match
                                    if (fresh_qualities.find(selected_quality) != fresh_qualities.end()) {
                                        fresh_playlist_url = fresh_qualities[selected_quality];
                                        selected_quality_name = selected_quality;
                                        quality_found = true;
                                        AddDebugLog(L"[AD_RECOVERY] Found exact quality match: " + selected_quality + L" for " + channel_name);
                                    }
                                    
                                    // If exact quality not found, try to find a similar quality
                                    if (!quality_found) {
                                        std::vector<std::wstring> priority_qualities = {L"source", L"1080p60", L"1080p", L"720p60", L"720p", L"480p", L"360p", L"160p"};
                                        for (const auto& priority_quality : priority_qualities) {
                                            if (fresh_qualities.find(priority_quality) != fresh_qualities.end()) {
                                                fresh_playlist_url = fresh_qualities[priority_quality];
                                                selected_quality_name = priority_quality;
                                                quality_found = true;
                                                AddDebugLog(L"[AD_RECOVERY] Using fallback quality: " + priority_quality + L" for " + channel_name);
                                                break;
                                            }
                                        }
                                    }
                                    
                                    // If still no match, find the first NON-AUDIO quality (avoid audio_only)
                                    if (!quality_found && !fresh_qualities.empty()) {
                                        for (const auto& quality_pair : fresh_qualities) {
                                            // Skip audio-only streams - we want video content
                                            if (quality_pair.first.find(L"audio") == std::wstring::npos && 
                                                quality_pair.first != L"audio_only" &&
                                                quality_pair.first.find(L"Audio") == std::wstring::npos) {
                                                fresh_playlist_url = quality_pair.second;
                                                selected_quality_name = quality_pair.first;
                                                quality_found = true;
                                                AddDebugLog(L"[AD_RECOVERY] Using first available video quality: " + quality_pair.first + L" for " + channel_name);
                                                break;
                                            }
                                        }
                                        
                                        // If somehow we only have audio streams available, log this as an error
                                        if (!quality_found) {
                                            AddDebugLog(L"[AD_RECOVERY] ERROR: Only audio streams available in fresh playlist for " + channel_name);
                                        }
                                    }
                                    
                                    // Additional validation: check if the URL looks like a video stream
                                    if (quality_found) {
                                        AddDebugLog(L"[AD_RECOVERY] Selected quality: " + selected_quality_name + L" -> URL: " + fresh_playlist_url);
                                        
                                        // Validate URL doesn't contain audio-only indicators
                                        if (fresh_playlist_url.find(L"audio") != std::wstring::npos ||
                                            fresh_playlist_url.find(L"Audio") != std::wstring::npos) {
                                            AddDebugLog(L"[AD_RECOVERY] WARNING: Selected URL appears to be audio-only based on URL content: " + fresh_playlist_url);
                                            quality_found = false;
                                            
                                            // Try to find a different quality that doesn't have audio in the URL
                                            for (const auto& quality_pair : fresh_qualities) {
                                                if (quality_pair.second.find(L"audio") == std::wstring::npos &&
                                                    quality_pair.second.find(L"Audio") == std::wstring::npos &&
                                                    quality_pair.first.find(L"audio") == std::wstring::npos &&
                                                    quality_pair.first != L"audio_only") {
                                                    fresh_playlist_url = quality_pair.second;
                                                    selected_quality_name = quality_pair.first;
                                                    quality_found = true;
                                                    AddDebugLog(L"[AD_RECOVERY] Found alternative video quality: " + quality_pair.first + L" -> " + quality_pair.second);
                                                    break;
                                                }
                                            }
                                        }
                                        
                                        // Additional validation: try to fetch a small sample of the media playlist to verify it contains video
                                        if (quality_found) {
                                            AddDebugLog(L"[AD_RECOVERY] Validating that selected playlist contains video content...");
                                            
                                            // Quick fetch to validate the media playlist structure
                                            std::string sample_resp;
                                            if (HttpGetText(fresh_playlist_url, sample_resp, &cancel_token)) {
                                                std::wstring sample_playlist = Utf8ToWide(sample_resp);
                                                
                                                if (!sample_playlist.empty()) {
                                                    // Check if this looks like a video media playlist
                                                    bool has_video_segments = sample_playlist.find(L".ts") != std::wstring::npos ||
                                                                             sample_playlist.find(L".m4s") != std::wstring::npos;
                                                    bool has_audio_only_markers = sample_playlist.find(L"AUDIO=") != std::wstring::npos &&
                                                                                  sample_playlist.find(L"VIDEO=") == std::wstring::npos;
                                                    
                                                    if (has_video_segments && !has_audio_only_markers) {
                                                        AddDebugLog(L"[AD_RECOVERY] ✓ Selected playlist appears to contain video segments");
                                                    } else {
                                                        AddDebugLog(L"[AD_RECOVERY] ✗ WARNING: Selected playlist might be audio-only (segments=" + 
                                                                   std::to_wstring(has_video_segments) + L", audio_only_markers=" + 
                                                                   std::to_wstring(has_audio_only_markers) + L")");
                                                        AddDebugLog(L"[AD_RECOVERY] Sample playlist content: " + sample_playlist.substr(0, 300));
                                                    }
                                                } else {
                                                    AddDebugLog(L"[AD_RECOVERY] WARNING: Could not fetch sample content from selected playlist URL");
                                                }
                                            } else {
                                                AddDebugLog(L"[AD_RECOVERY] WARNING: Failed to fetch sample content from selected playlist URL");
                                            }
                                        }
                                    }
                                    
                                    if (quality_found) {
                                        // Update the media playlist URL to the fresh one
                                        media_playlist_url = fresh_playlist_url;
                                        AddDebugLog(L"[AD_RECOVERY] Successfully switched to fresh playlist URL to bypass ads for " + channel_name);
                                        AddDebugLog(L"[AD_RECOVERY] New playlist URL: " + fresh_playlist_url);
                                        
                                        // Update fresh playlist tracking
                                        fresh_playlist_attempts++;
                                        last_fresh_playlist_time = current_time;
                                        
                                        // Clear seen URLs to allow re-downloading from fresh playlist
                                        seen_urls.clear();
                                        // Don't re-insert __FETCH_FRESH_PLAYLIST__ marker to allow future processing
                                        
                                        // Reset error count since we're starting fresh
                                        consecutive_errors = 0;
                                        
                                        // Break from segment processing to restart playlist fetching with new URL
                                        AddDebugLog(L"[AD_RECOVERY] Breaking from segment loop to restart with fresh playlist for " + channel_name);
                                        break;
                                    } else {
                                        AddDebugLog(L"[AD_RECOVERY] Warning: Could not find suitable quality in fresh playlist for " + channel_name);
                                    }
                                } else {
                                    AddDebugLog(L"[AD_RECOVERY] Warning: Fresh playlist contains no quality information for " + channel_name);
                                }
                            } else {
                                AddDebugLog(L"[AD_RECOVERY] Failed to fetch fresh master playlist for " + channel_name);
                            }
                        } else if (fresh_access_token == L"OFFLINE") {
                            AddDebugLog(L"[AD_RECOVERY] Channel " + channel_name + L" is offline, cannot fetch fresh playlist");
                        } else {
                            AddDebugLog(L"[AD_RECOVERY] Failed to obtain fresh access token for " + channel_name);
                        }
                    } else {
                        AddDebugLog(L"[AD_RECOVERY] Warning: Missing channel name or selected quality for ad recovery");
                    }
                    
                    // Fallback: if fresh playlist fetch failed, just continue without reset
                    AddDebugLog(L"[AD_RECOVERY] Continuing with existing playlist for " + channel_name);
                    continue;
                }
                
                // Check if this is a regular segment we've already seen
                if (seen_urls.count(seg)) continue;
                
                // Check buffer size before downloading more (unless urgent download is needed)
                size_t current_buffer_size;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    current_buffer_size = buffer_queue.size();
                }
                
                bool urgent_bypass = urgent_download_needed.load();
                if (current_buffer_size >= max_buffer_segments && !urgent_bypass) {
                    auto current_time = std::chrono::steady_clock::now();
                    
                    // Start timing if this is the first time buffer is full
                    if (!buffer_full_timer_active) {
                        buffer_full_timer_active = true;
                        buffer_full_start_time = current_time;
                        AddDebugLog(L"[BUFFER] Buffer full (" + std::to_wstring(current_buffer_size) + 
                                   L"), starting timeout timer for " + channel_name);
                    } else {
                        // Check if buffer has been full too long
                        auto duration_full = std::chrono::duration_cast<std::chrono::seconds>(current_time - buffer_full_start_time);
                        if (duration_full.count() >= buffer_full_timeout_seconds) {
                            // Empty the buffer to prevent stagnation
                            size_t cleared_segments;
                            {
                                std::lock_guard<std::mutex> lock(buffer_mutex);
                                cleared_segments = buffer_queue.size();
                                std::queue<std::vector<char>> empty_queue;
                                buffer_queue.swap(empty_queue); // Clear the queue efficiently
                            }
                            
                            buffer_full_timer_active = false; // Reset timer
                            
                            AddDebugLog(L"[BUFFER] Buffer full timeout (" + std::to_wstring(duration_full.count()) + 
                                       L"s) - cleared " + std::to_wstring(cleared_segments) + 
                                       L" segments for " + channel_name);
                            continue; // Skip the wait and try downloading again
                        } else {
                            AddDebugLog(L"[BUFFER] Buffer full (" + std::to_wstring(current_buffer_size) + 
                                       L"), waiting (" + std::to_wstring(duration_full.count()) + 
                                       L"/" + std::to_wstring(buffer_full_timeout_seconds) + L"s) for " + channel_name);
                        }
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                } else {
                    // Buffer is not full, reset the timer
                    if (buffer_full_timer_active) {
                        buffer_full_timer_active = false;
                        AddDebugLog(L"[BUFFER] Buffer no longer full, resetting timeout timer for " + channel_name);
                    }
                    
                    // Log if urgent download is bypassing normal buffer checks
                    if (urgent_bypass) {
                        AddDebugLog(L"[DOWNLOAD] Urgent download bypassing buffer fullness check (buffer=" + 
                                   std::to_wstring(current_buffer_size) + L"/" + std::to_wstring(max_buffer_segments) + L") for " + channel_name);
                    }
                }
                
                seen_urls.insert(seg);
                std::wstring seg_url = JoinUrl(media_playlist_url, seg);
                std::vector<char> seg_data;
                
                // Download with retries
                bool download_ok = false;
                for (int retry = 0; retry < 3; ++retry) {
                    if (HttpGetBinary(seg_url, seg_data, 1, &cancel_token)) {
                        download_ok = true;
                        break;
                    }
                    if (!download_running || cancel_token.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }

                if (download_ok && !seg_data.empty()) {
                    // Add to buffer
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        buffer_queue.push(std::move(seg_data));
                    }
                    new_segments_downloaded++;
                    AddDebugLog(L"[DOWNLOAD] Downloaded segment " + std::to_wstring(new_segments_downloaded) + 
                               L", buffer=" + std::to_wstring(current_buffer_size + 1) + L" for " + channel_name);
                } else {
                    AddDebugLog(L"[DOWNLOAD] FAILED to download segment after retries for " + channel_name);
                }
            }
            
            AddDebugLog(L"[DOWNLOAD] Segment batch complete - downloaded " + std::to_wstring(new_segments_downloaded) + 
                       L" new segments for " + channel_name);
            
            // Poll playlist more frequently for live streams or when urgent download is needed
            urgent_needed = urgent_download_needed.load();
            if (urgent_needed) {
                urgent_download_needed.store(false); // Reset the flag
                AddDebugLog(L"[DOWNLOAD] Urgent download completed, immediately fetching next playlist for " + channel_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Very short delay for urgent downloads
            } else {
                AddDebugLog(L"[DOWNLOAD] Sleeping 1.5s before next playlist fetch for " + channel_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }
        }
        
        // Log exactly why the download loop ended
        AddDebugLog(L"[DOWNLOAD] *** DOWNLOAD THREAD ENDING *** for " + channel_name);
        AddDebugLog(L"[DOWNLOAD] Exit conditions: download_running=" + std::to_wstring(download_running.load()) +
                   L", cancel_token=" + std::to_wstring(cancel_token.load()) +
                   L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess, channel_name + L" final_check", pi.dwProcessId)) +
                   L", consecutive_errors=" + std::to_wstring(consecutive_errors) + L"/" + std::to_wstring(max_consecutive_errors) +
                   L", stream_ended_normally=" + std::to_wstring(stream_ended_normally.load()));
        
        // Additional detailed process checking
        DWORD exit_code = 0;
        BOOL got_exit_code = GetExitCodeProcess(pi.hProcess, &exit_code);
        AddDebugLog(L"[DOWNLOAD] Process details: GetExitCodeProcess=" + std::to_wstring(got_exit_code) +
                   L", ExitCode=" + std::to_wstring(exit_code) + 
                   L", STILL_ACTIVE=" + std::to_wstring(STILL_ACTIVE) +
                   L", PID=" + std::to_wstring(pi.dwProcessId));
    });

    // Main buffer feeding thread - writes directly to player stdin pipe with anti-freezing mechanisms
    std::thread feeder_thread([&]() {
        auto feeder_start_time = std::chrono::high_resolution_clock::now();
        auto feeder_startup_delay = std::chrono::duration_cast<std::chrono::milliseconds>(feeder_start_time - thread_start_time);
        
        bool started = false;
        const size_t min_buffer_size = 3; // Minimum 3 segments for smooth streaming
        int empty_buffer_count = 0;
        const int max_empty_waits = 100; // 5 seconds max wait for data (50ms * 100)
        
        AddDebugLog(L"[FEEDER] Starting IPC feeder thread for " + channel_name + 
                   L", startup_delay=" + std::to_wstring(feeder_startup_delay.count()) + L"ms");
        
        while (true) {
            // Check all exit conditions individually for detailed logging  
            bool cancel_token_check = cancel_token.load();
            bool process_running_check = ProcessStillRunning(pi.hProcess, channel_name + L" feeder_thread", pi.dwProcessId);
            bool data_available_check = (download_running.load() || !buffer_queue.empty());
            
            if (cancel_token_check) {
                AddDebugLog(L"[FEEDER] Exit condition: cancel_token=true for " + channel_name);
                break;
            }
            if (!process_running_check) {
                AddDebugLog(L"[FEEDER] Exit condition: process died for " + channel_name);
                break;
            }
            if (!data_available_check) {
                AddDebugLog(L"[FEEDER] Exit condition: no more data available (download stopped and buffer empty) for " + channel_name);
                break;
            }
            
            size_t buffer_size;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                buffer_size = buffer_queue.size();
            }
            
            // Wait for initial buffer before starting - increased for stability
            if (!started) {
                if (buffer_size >= target_buffer_segments) {
                    started = true;
                    AddDebugLog(L"[FEEDER] Initial buffer ready (" + 
                               std::to_wstring(buffer_size) + L" segments), starting IPC feed for " + channel_name);
                } else {
                    AddDebugLog(L"[FEEDER] Waiting for initial buffer (" + std::to_wstring(buffer_size) + L"/" +
                               std::to_wstring(target_buffer_segments) + L") for " + channel_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Multi-segment feeding to maintain continuous flow
            std::vector<std::vector<char>> segments_to_feed;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                
                // Feed multiple segments when buffer is low to prevent freezing
                int max_segments_to_feed = 1;
                if (buffer_size < min_buffer_size) {
                    // When buffer is low, be more aggressive about feeding segments
                    max_segments_to_feed = std::min((int)buffer_queue.size(), 3); // Feed up to 3 segments when low
                    
                    AddDebugLog(L"[FEEDER] Buffer low (" + std::to_wstring(buffer_size) + 
                               L" < " + std::to_wstring(min_buffer_size) + 
                               L"), feeding " + std::to_wstring(max_segments_to_feed) + L" segments for " + channel_name);
                    
                    // Warning if buffer reaches 0
                    if (buffer_size == 0) {
                        AddDebugLog(L"[FEEDER] *** WARNING: Buffer reached 0 for " + channel_name + L" ***");
                        // Signal download thread to urgently fetch more segments
                        urgent_download_needed.store(true);
                        AddDebugLog(L"[FEEDER] Triggered urgent download to refill empty buffer for " + channel_name);
                    }
                }
                
                int segments_fed = 0;
                while (!buffer_queue.empty() && segments_fed < max_segments_to_feed) {
                    segments_to_feed.push_back(std::move(buffer_queue.front()));
                    buffer_queue.pop();
                    segments_fed++;
                }
            }
            
            if (!segments_to_feed.empty()) {
                // Write segments directly to player stdin pipe with retry logic
                bool write_failed = false;
                int segments_processed = 0;
                
                for (const auto& segment_data : segments_to_feed) {
                    // Skip empty segments (shouldn't happen with buffer reset approach)
                    if (segment_data.empty()) {
                        AddDebugLog(L"[IPC] Warning: Found empty segment in buffer for " + channel_name);
                        segments_processed++;
                        continue;
                    }
                    
                    bool segment_written = false;
                    int write_attempts = 0;
                    const int max_write_attempts = 3;
                    
                    while (!segment_written && write_attempts < max_write_attempts && !cancel_token.load()) {
                        DWORD bytes_written = 0;
                        // Use WriteFile with timeout to prevent slow writes from blocking indefinitely
                        BOOL write_result = WriteFileWithTimeout(stdin_pipe, segment_data.data(), (DWORD)segment_data.size(), &bytes_written, 3000); // 3 second timeout
                        
                        if (write_result && bytes_written == segment_data.size()) {
                            segment_written = true;
                            segments_processed++;
                        } else {
                            write_attempts++;
                            DWORD error = GetLastError();
                            
                            if (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE) {
                                AddDebugLog(L"[IPC] Pipe closed/broken for " + channel_name + L", error=" + std::to_wstring(error));
                                write_failed = true;
                                break; // Pipe is broken, no point retrying
                            }
                            
                            if (error == ERROR_TIMEOUT) {
                                AddDebugLog(L"[IPC] Write timeout (3s) on attempt " + std::to_wstring(write_attempts) + 
                                           L"/" + std::to_wstring(max_write_attempts) + L" for " + channel_name + 
                                           L" - slow write detected");
                            } else {
                                AddDebugLog(L"[IPC] Write attempt " + std::to_wstring(write_attempts) + L"/" + std::to_wstring(max_write_attempts) + 
                                           L" failed for " + channel_name + 
                                           L", Error=" + std::to_wstring(error) + 
                                           L", BytesWritten=" + std::to_wstring(bytes_written) + 
                                           L"/" + std::to_wstring(segment_data.size()));
                            }
                            
                            if (write_attempts < max_write_attempts) {
                                // Wait briefly before retry to handle temporary write issues
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }
                    }
                    
                    if (!segment_written) {
                        AddDebugLog(L"[IPC] Failed to write segment after " + std::to_wstring(max_write_attempts) + L" attempts for " + channel_name);
                        write_failed = true;
                        break;
                    }
                }
                
                if (write_failed) {
                    AddDebugLog(L"[IPC] Write failure detected, stopping feeder for " + channel_name);
                    break;
                }
                
                size_t remaining_buffer = buffer_size - segments_to_feed.size();
                if (chunk_count) {
                    chunk_count->store((int)remaining_buffer);
                }
                
                empty_buffer_count = 0; // Reset counter when data is fed
                AddDebugLog(L"[IPC] Fed " + std::to_wstring(segments_processed) + 
                           L" segments to " + channel_name + L", buffer=" + std::to_wstring(remaining_buffer));
                
                // Shorter wait when actively feeding to maintain flow
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                // No data available - wait for more data
                empty_buffer_count++;
                if (empty_buffer_count >= max_empty_waits) {
                    AddDebugLog(L"[IPC] No data for too long (" + std::to_wstring(empty_buffer_count * 50) + 
                               L"ms), ending stream for " + channel_name);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        
        AddDebugLog(L"[FEEDER] IPC feeder thread ending for " + channel_name +
                   L", cancel=" + std::to_wstring(cancel_token.load()) + 
                   L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess, channel_name + L" feeder_final", pi.dwProcessId)) +
                   L", download_running=" + std::to_wstring(download_running.load()) +
                   L", buffer_queue_empty=" + std::to_wstring(buffer_queue.empty()) +
                   L", empty_buffer_count=" + std::to_wstring(empty_buffer_count));
    });

    // Wait for download and feeder threads to complete
    download_thread.join();
    download_running = false;
    feeder_thread.join();

    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup starting for " + channel_name + 
               L", cancel=" + std::to_wstring(cancel_token.load()) + 
               L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess, channel_name + L" cleanup_check", pi.dwProcessId)) +
               L", stream_ended_normally=" + std::to_wstring(stream_ended_normally.load()));

    // Close stdin pipe to signal end of stream to player
    CloseHandle(stdin_pipe);
    
    // Allow time for player to process remaining data
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Cleanup process
    if (ProcessStillRunning(pi.hProcess, channel_name + L" termination_check", pi.dwProcessId)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Terminating player process for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    
    // Decrement active stream counter
    int remaining_streams;
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        remaining_streams = --g_active_streams;
    }
    
    bool normal_end = stream_ended_normally.load();
    bool user_cancel = cancel_token.load();
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup complete for " + channel_name);
    
    AddDebugLog(L"[STREAMS] Stream ended - " + std::to_wstring(remaining_streams) + L" streams remain active");
    AddDebugLog(L"[STREAMS] Exit reason: normal_end=" + std::to_wstring(normal_end) + 
               L", user_cancel=" + std::to_wstring(user_cancel) + L" for " + channel_name);
    
    // If user explicitly cancelled or stream ended normally, or if the process
    // was terminated (which could be user closing the player), consider it successful
    return normal_end || user_cancel || !ProcessStillRunning(pi.hProcess, channel_name + L" final_return_check", pi.dwProcessId);
}