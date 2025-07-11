#include "stream_pipe.h"
#include "stream_thread.h"
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
        
        // Stream data from queue with continuous flow to prevent freezing
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
                int sent = send(client_socket, data.data(), (int)data.size(), 0);
                if (sent <= 0) {
                    AddDebugLog(L"[HTTP] Client disconnected after " + std::to_wstring(segments_sent) + L" segments");
                    break; // Client disconnected
                }
                segments_sent++;
            } else {
                // No data available - use shorter wait to prevent video freezing
                empty_queue_count++;
                if (empty_queue_count >= max_empty_waits) {
                    AddDebugLog(L"[HTTP] No data for too long, ending client session to prevent freeze");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Reduced from 50ms to 10ms
            }
        }
        
        AddDebugLog(L"[HTTP] Client session ended, sent " + std::to_wstring(segments_sent) + L" segments");
        closesocket(client_socket);
    }
};

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
            // Skip this segment as it contains ad content
            AddDebugLog(L"[FILTER] Skipping ad segment: " + std::wstring(line.begin(), line.end()));
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
    std::atomic<int>* chunk_count
) {
    // Track active streams for cross-stream interference detection
    int current_stream_count;
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        current_stream_count = ++g_active_streams;
    }
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting robust HTTP streaming for " + channel_name + L", URL=" + playlist_url);
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

    // Add startup delay for multi-stream scenarios to reduce resource contention
    int stream_count = g_active_streams.load();
    if (stream_count > 1) {
        int delay_ms = (stream_count - 1) * 500; // 500ms delay per additional stream
        AddDebugLog(L"[ISOLATION] Adding " + std::to_wstring(delay_ms) + L"ms startup delay for stream #" + 
                   std::to_wstring(stream_count) + L" (" + channel_name + L")");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    // 2. Create and start HTTP server with detailed port allocation logging
    StreamHttpServer http_server;
    AddDebugLog(L"[HTTP] Attempting to start HTTP server for " + channel_name + L", preferred port=8080");
    if (!http_server.Start(8080)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to start HTTP server for " + channel_name);
        // Decrement stream counter on early exit
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        --g_active_streams;
        return false;
    }
    
    int server_port = http_server.GetPort();
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Started HTTP server on port " + std::to_wstring(server_port) + L" for " + channel_name);
    
    // Log if we got a different port than expected (indicates port conflicts)
    if (server_port != 8080) {
        AddDebugLog(L"[HTTP] Port conflict resolved - got port " + std::to_wstring(server_port) + 
                   L" instead of 8080 for " + channel_name);
    }

    // 3. Create media player process with localhost URL and proper isolation
    std::wstring localhost_url = L"http://localhost:" + std::to_wstring(server_port) + L"/";
    
    // Create isolated working directory for this stream to prevent conflicts
    std::wstring temp_dir = L"C:\\Temp\\Tardsplaya_" + channel_name + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
    CreateDirectoryW(temp_dir.c_str(), nullptr);
    
    // Build command with media player isolation parameters
    std::wstring cmd;
    if (player_path.find(L"mpc-hc") != std::wstring::npos) {
        // MPC-HC specific isolation: use different config path and disable instance management
        cmd = L"\"" + player_path + L"\" \"" + localhost_url + L"\" /new /nofocus";
    } else if (player_path.find(L"vlc") != std::wstring::npos) {
        // VLC specific isolation: use different config directory
        std::wstring vlc_config = temp_dir + L"\\vlc_config";
        CreateDirectoryW(vlc_config.c_str(), nullptr);
        cmd = L"\"" + player_path + L"\" \"" + localhost_url + L"\" --intf dummy --no-one-instance --config=" + vlc_config;
    } else {
        // Generic media player - basic isolation
        cmd = L"\"" + player_path + L"\" \"" + localhost_url + L"\"";
    }
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Launching isolated player: " + cmd + L" for " + channel_name);
    AddDebugLog(L"[ISOLATION] Working directory: " + temp_dir + L" for " + channel_name);
    
    // Record timing for process creation
    auto start_time = std::chrono::high_resolution_clock::now();
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, temp_dir.c_str(), &si, &pi
    );
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    if (!success) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create process for " + channel_name + 
                   L", Error=" + std::to_wstring(error) + L", Duration=" + std::to_wstring(duration.count()) + L"ms");
        // Clean up temp directory on failure
        if (!temp_dir.empty()) {
            std::wstring rmdir_cmd = L"rmdir /s /q \"" + temp_dir + L"\"";
            _wsystem(rmdir_cmd.c_str());
        }
        // Decrement stream counter on early exit
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        --g_active_streams;
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Process created successfully for " + channel_name + 
               L", PID=" + std::to_wstring(pi.dwProcessId) + L", Duration=" + std::to_wstring(duration.count()) + L"ms");
    
    // Set process priority for better multi-stream performance
    stream_count = g_active_streams.load(); // Update count before setting priority
    if (stream_count > 1) {
        // Lower priority for additional streams to reduce conflicts
        SetPriorityClass(pi.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
        AddDebugLog(L"[ISOLATION] Set BELOW_NORMAL priority for stream #" + std::to_wstring(stream_count) + L" (" + channel_name + L")");
    } else {
        // Normal priority for first stream
        SetPriorityClass(pi.hProcess, NORMAL_PRIORITY_CLASS);
        AddDebugLog(L"[ISOLATION] Set NORMAL priority for first stream (" + channel_name + L")");
    }
    
    // Verify process is actually running immediately after creation
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Brief delay for process startup
    bool initial_check = ProcessStillRunning(pi.hProcess, channel_name + L" initial_verification", pi.dwProcessId);
    AddDebugLog(L"[PROCESS] Initial verification after 100ms: " + std::to_wstring(initial_check) + L" for " + channel_name);

    // 4. Start thread to maintain player window title with channel name
    std::thread title_thread([pi, channel_name]() {
        SetPlayerWindowTitle(pi.dwProcessId, channel_name);
    });
    title_thread.detach();

    // 5. Robust streaming with background download threads and persistent buffering
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::set<std::wstring> seen_urls;
    std::atomic<bool> download_running(true);
    std::atomic<bool> stream_ended_normally(false);
    
    // Store temp directory for cleanup
    std::wstring temp_dir_for_cleanup = temp_dir;
    
    const int target_buffer_segments = std::max(buffer_segments, 10); // Minimum 10 segments
    const int max_buffer_segments = target_buffer_segments * 2; // Don't over-buffer
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Target buffer: " + std::to_wstring(target_buffer_segments) + 
               L" segments, max: " + std::to_wstring(max_buffer_segments) + L" for " + channel_name);

    // Log thread creation timing
    auto thread_start_time = std::chrono::high_resolution_clock::now();
    AddDebugLog(L"[THREAD] Creating download and feeder threads for " + channel_name);

    // Background playlist monitor and segment downloader thread
    std::thread download_thread([&]() {
        auto thread_actual_start = std::chrono::high_resolution_clock::now();
        auto startup_delay = std::chrono::duration_cast<std::chrono::milliseconds>(thread_actual_start - thread_start_time);
        
        int consecutive_errors = 0;
        const int max_consecutive_errors = 15; // ~30 seconds (2 sec intervals)
        
        AddDebugLog(L"[DOWNLOAD] Starting download thread for " + channel_name + 
                   L", startup_delay=" + std::to_wstring(startup_delay.count()) + L"ms");
        
        while (true) {
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
            AddDebugLog(L"[DOWNLOAD] Parsed " + std::to_wstring(segments.size()) + 
                       L" segments from playlist for " + channel_name);
            
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
                
                if (seen_urls.count(seg)) continue;
                
                // Check buffer size before downloading more
                size_t current_buffer_size;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    current_buffer_size = buffer_queue.size();
                }
                
                if (current_buffer_size >= max_buffer_segments) {
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Buffer full (" + std::to_wstring(current_buffer_size) + 
                               L"), waiting for " + channel_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
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
            
            // Poll playlist more frequently for live streams
            AddDebugLog(L"[DOWNLOAD] Sleeping 1.5s before next playlist fetch for " + channel_name);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
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

    // Main buffer feeding thread - keeps HTTP server well-fed
    std::thread feeder_thread([&]() {
        auto feeder_start_time = std::chrono::high_resolution_clock::now();
        auto feeder_startup_delay = std::chrono::duration_cast<std::chrono::milliseconds>(feeder_start_time - thread_start_time);
        
        bool started = false;
        AddDebugLog(L"[FEEDER] Starting feeder thread for " + channel_name + 
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
                    // Ensure HTTP server has adequate initial buffer to prevent freezing
                    size_t http_buffer = http_server.GetQueueLength();
                    if (http_buffer < 2) {
                        // Pre-feed HTTP server with a couple segments
                        int prefeed_count = 0;
                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        while (!buffer_queue.empty() && prefeed_count < 3) {
                            std::vector<char> prefeed_data = std::move(buffer_queue.front());
                            buffer_queue.pop();
                            http_server.AddData(prefeed_data);
                            prefeed_count++;
                        }
                        AddDebugLog(L"[FEEDER] Pre-fed " + std::to_wstring(prefeed_count) + 
                                   L" segments to HTTP server for startup stability for " + channel_name);
                    }
                    
                    started = true;
                    AddDebugLog(L"[FEEDER] Initial buffer ready (" + 
                               std::to_wstring(buffer_size) + L" segments), starting feed for " + channel_name);
                } else {
                    AddDebugLog(L"[FEEDER] Waiting for initial buffer (" + std::to_wstring(buffer_size) + L"/" +
                               std::to_wstring(target_buffer_segments) + L") for " + channel_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Feed multiple segments to HTTP server to maintain continuous flow
            std::vector<std::vector<char>> segments_to_feed;
            size_t http_queue_size = http_server.GetQueueLength();
            size_t min_queue_size = http_server.GetMinQueueSize();
            
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                // Feed segments based on HTTP server queue size to maintain minimum
                int max_segments_to_feed = std::max(1, (int)(min_queue_size - http_queue_size + 1));
                int segments_fed = 0;
                
                while (!buffer_queue.empty() && segments_fed < max_segments_to_feed) {
                    segments_to_feed.push_back(std::move(buffer_queue.front()));
                    buffer_queue.pop();
                    segments_fed++;
                }
            }
            
            if (!segments_to_feed.empty()) {
                for (auto& segment_data : segments_to_feed) {
                    http_server.AddData(segment_data);
                }
                
                size_t new_http_buffer = http_server.GetQueueLength();
                size_t current_buffer = buffer_size - segments_to_feed.size();
                AddDebugLog(L"[FEEDER] Fed " + std::to_wstring(segments_to_feed.size()) + 
                           L" segments to HTTP server, local_buffer=" + std::to_wstring(current_buffer) + 
                           L", http_buffer=" + std::to_wstring(new_http_buffer) + L" for " + channel_name);
                
                if (chunk_count) {
                    *chunk_count = (int)current_buffer;
                }
                
                // Shorter wait when actively feeding
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                // No segments available, wait
                AddDebugLog(L"[FEEDER] No segments available, waiting... (download_running=" + 
                           std::to_wstring(download_running.load()) + L") for " + channel_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        
        AddDebugLog(L"[FEEDER] *** FEEDER THREAD ENDING *** for " + channel_name +
                   L", cancel=" + std::to_wstring(cancel_token.load()) + 
                   L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess, channel_name + L" feeder_final", pi.dwProcessId)) +
                   L", download_running=" + std::to_wstring(download_running.load()) +
                   L", buffer_queue_empty=" + std::to_wstring(buffer_queue.empty()));
    });

    // Wait for download and feeder threads to complete
    download_thread.join();
    download_running = false;
    feeder_thread.join();

    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup starting for " + channel_name + 
               L", cancel=" + std::to_wstring(cancel_token.load()) + 
               L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess, channel_name + L" cleanup_check", pi.dwProcessId)) +
               L", stream_ended_normally=" + std::to_wstring(stream_ended_normally.load()));

    // Signal stream end to HTTP server
    http_server.SetStreamEnded();
    
    // Allow time for final data to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Stop HTTP server
    http_server.Stop();

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
    
    // Clean up isolated temp directory
    if (!temp_dir_for_cleanup.empty()) {
        AddDebugLog(L"[ISOLATION] Cleaning up temp directory: " + temp_dir_for_cleanup + L" for " + channel_name);
        // Use system command to remove directory recursively 
        std::wstring rmdir_cmd = L"rmdir /s /q \"" + temp_dir_for_cleanup + L"\"";
        _wsystem(rmdir_cmd.c_str());
    }
    
    AddDebugLog(L"[STREAMS] Stream ended - " + std::to_wstring(remaining_streams) + L" streams remain active");
    AddDebugLog(L"[STREAMS] Exit reason: normal_end=" + std::to_wstring(normal_end) + 
               L", user_cancel=" + std::to_wstring(user_cancel) + L" for " + channel_name);
    
    return normal_end || user_cancel;
}