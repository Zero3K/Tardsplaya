#include "stream_pipe.h"
#include "stream_thread.h"
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
#include <queue>
#include <mutex>
#include "tlsclient/tlsclient.h"

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
        
        // Stream data from queue with better buffering
        while (running && !stream_ended) {
            std::vector<char> data;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!data_queue.empty()) {
                    data = std::move(data_queue.front());
                    data_queue.pop();
                    buffer_size -= data.size();
                }
            }
            
            if (!data.empty()) {
                int sent = send(client_socket, data.data(), (int)data.size(), 0);
                if (sent <= 0) break; // Client disconnected
            } else {
                // No data available, wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        
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

// Parse media segment URLs from m3u8 playlist
static std::vector<std::wstring> ParseSegments(const std::string& playlist) {
    std::vector<std::wstring> segs;
    std::istringstream ss(playlist);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
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
            AddDebugLog(L"ProcessStillRunning: Invalid handle for " + debug_context);
        }
        return false;
    }
    
    DWORD code = 0;
    BOOL result = GetExitCodeProcess(hProcess, &code);
    bool still_active = result && (code == STILL_ACTIVE);
    
    // Double-check using PID if we have it
    if (!still_active && pid != 0) {
        HANDLE pidHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (pidHandle != nullptr) {
            DWORD pidCode = 0;
            BOOL pidResult = GetExitCodeProcess(pidHandle, &pidCode);
            bool pidActive = pidResult && (pidCode == STILL_ACTIVE);
            CloseHandle(pidHandle);
            
            if (pidActive && !still_active) {
                AddDebugLog(L"ProcessStillRunning: Handle check failed but PID check succeeded for " + debug_context +
                           L", may be handle corruption");
                return true; // Trust PID check over handle check
            }
        }
    }
    
    if (!debug_context.empty() && !still_active) {
        AddDebugLog(L"ProcessStillRunning: Process NOT running for " + debug_context + 
                   L", GetExitCodeProcess=" + std::to_wstring(result) + 
                   L", ExitCode=" + std::to_wstring(code) + 
                   L", STILL_ACTIVE=" + std::to_wstring(STILL_ACTIVE) +
                   L", LastError=" + std::to_wstring(GetLastError()) +
                   L", PID=" + std::to_wstring(pid));
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
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting robust HTTP streaming for " + channel_name + L", URL=" + playlist_url);
    
    // 1. Download the master playlist and pick the first media playlist
    std::string master;
    if (cancel_token.load()) return false;
    if (!HttpGetText(playlist_url, master, &cancel_token)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to download master playlist for " + channel_name);
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

    // 2. Create and start HTTP server
    StreamHttpServer http_server;
    if (!http_server.Start(8080)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to start HTTP server for " + channel_name);
        return false;
    }
    
    int server_port = http_server.GetPort();
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Started HTTP server on port " + std::to_wstring(server_port) + L" for " + channel_name);

    // 3. Create media player process with localhost URL
    std::wstring localhost_url = L"http://localhost:" + std::to_wstring(server_port) + L"/";
    std::wstring cmd = L"\"" + player_path + L"\" \"" + localhost_url + L"\"";
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Launching player: " + cmd + L" for " + channel_name);
    
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi
    );

    if (!success) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create process for " + channel_name + 
                   L", Error=" + std::to_wstring(GetLastError()));
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Process created successfully for " + channel_name + 
               L", PID=" + std::to_wstring(pi.dwProcessId));

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
    
    const int target_buffer_segments = std::max(buffer_segments, 10); // Minimum 10 segments
    const int max_buffer_segments = target_buffer_segments * 2; // Don't over-buffer
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Target buffer: " + std::to_wstring(target_buffer_segments) + 
               L" segments, max: " + std::to_wstring(max_buffer_segments) + L" for " + channel_name);

    // Background playlist monitor and segment downloader thread
    std::thread download_thread([&]() {
        int consecutive_errors = 0;
        const int max_consecutive_errors = 15; // ~30 seconds (2 sec intervals)
        
        while (download_running && !cancel_token.load() && 
               ProcessStillRunning(pi.hProcess, channel_name + L" download_thread", pi.dwProcessId) && 
               consecutive_errors < max_consecutive_errors) {
            
            // Download current playlist
            std::string playlist;
            if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
                consecutive_errors++;
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Download thread playlist error " + 
                           std::to_wstring(consecutive_errors) + L"/" + 
                           std::to_wstring(max_consecutive_errors) + L" for " + channel_name);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            consecutive_errors = 0;

            // Check for stream end
            if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Download thread found stream end for " + channel_name);
                stream_ended_normally = true;
                break;
            }

            auto segments = ParseSegments(playlist);
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Download thread found " + std::to_wstring(segments.size()) + 
                       L" segments for " + channel_name);
            
            // Download new segments
            for (auto& seg : segments) {
                if (!download_running || cancel_token.load() || !ProcessStillRunning(pi.hProcess, channel_name + L" segment_download", pi.dwProcessId)) break;
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
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Downloaded segment, buffer=" + 
                               std::to_wstring(current_buffer_size + 1) + L" for " + channel_name);
                } else {
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to download segment for " + channel_name);
                }
            }
            
            // Poll playlist more frequently for live streams
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Download thread ending for " + channel_name + 
                   L", download_running=" + std::to_wstring(download_running.load()) +
                   L", cancel_token=" + std::to_wstring(cancel_token.load()) +
                   L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess, channel_name + L" final_check", pi.dwProcessId)) +
                   L", consecutive_errors=" + std::to_wstring(consecutive_errors) + L"/" + std::to_wstring(max_consecutive_errors));
    });

    // Main buffer feeding thread - keeps HTTP server well-fed
    std::thread feeder_thread([&]() {
        bool started = false;
        
        while (!cancel_token.load() && 
               ProcessStillRunning(pi.hProcess, channel_name + L" feeder_thread", pi.dwProcessId) && 
               (download_running || !buffer_queue.empty())) {
            
            size_t buffer_size;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                buffer_size = buffer_queue.size();
            }
            
            // Wait for initial buffer before starting
            if (!started) {
                if (buffer_size >= target_buffer_segments) {
                    started = true;
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Initial buffer ready (" + 
                               std::to_wstring(buffer_size) + L" segments), starting feed for " + channel_name);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Feed segments to HTTP server
            std::vector<char> segment_data;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!buffer_queue.empty()) {
                    segment_data = std::move(buffer_queue.front());
                    buffer_queue.pop();
                }
            }
            
            if (!segment_data.empty()) {
                http_server.AddData(segment_data);
                size_t http_buffer = http_server.GetQueueLength();
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Fed segment to HTTP server, local_buffer=" + 
                           std::to_wstring(buffer_size - 1) + L", http_buffer=" + 
                           std::to_wstring(http_buffer) + L" for " + channel_name);
                
                if (chunk_count) {
                    *chunk_count = (int)buffer_size - 1;
                }
            } else {
                // No segments available, wait
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Feeder thread ending for " + channel_name);
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
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup complete for " + channel_name);
    return stream_ended_normally.load() || cancel_token.load();
}