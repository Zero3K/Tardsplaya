#include "stream_pipe.h"
#include "stream_thread.h"
#include "stream_resource_manager.h"
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
    bool has_valid_header = false;
    bool has_segments = false;
    
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            // Check for valid HLS header
            if (line.find("#EXTM3U") == 0) {
                has_valid_header = true;
            }
            // Check for HLS stream end indicator - but only if we have a valid playlist
            if (line.find("#EXT-X-ENDLIST") == 0 && has_valid_header) {
                ended = true;
            }
            continue;
        }
        // Should be a .ts or .aac segment
        std::wstring wline(line.begin(), line.end());
        segs.push_back(wline);
        has_segments = true;
    }
    
    // Only consider the stream ended if we have a valid playlist with segments
    // and the end marker is present - this prevents false positives from partial downloads
    if (is_ended) *is_ended = ended && has_valid_header && (has_segments || ended);
    return segs;
}

// Returns true if process handle is still alive - uses resource manager for multi-stream tolerance
static bool ProcessStillRunning(HANDLE hProcess, const std::wstring& channel_name = L"") {
    return StreamProcessUtils::IsProcessGenuinelyRunning(hProcess, channel_name);
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
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting for " + channel_name + 
               L", PlayerPath=" + player_path);
    
    // Use resource manager for proper multi-stream handling
    StreamResourceQuota quota;
    StreamResourceGuard resource_guard(channel_name, quota);
    
    if (!resource_guard.IsValid()) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create resource guard for " + channel_name);
        return false;
    }
    
    // Get system-appropriate delays and settings
    auto& resource_manager = StreamResourceManager::getInstance();
    DWORD start_delay = resource_manager.GetRecommendedStartDelay();
    DWORD pipe_buffer = resource_manager.GetRecommendedPipeBuffer();
    DWORD process_priority = resource_manager.GetRecommendedProcessPriority();
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Resource settings for " + channel_name + 
               L", delay=" + std::to_wstring(start_delay) + 
               L", buffer=" + std::to_wstring(pipe_buffer) + 
               L", priority=" + std::to_wstring(process_priority));
    
    // Apply recommended start delay to spread out resource usage
    if (start_delay > 0) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Applying start delay " + 
                   std::to_wstring(start_delay) + L"ms for " + channel_name);
        std::this_thread::sleep_for(std::chrono::milliseconds(start_delay));
    }
    
    // 1. Download the master playlist and pick the first media playlist (or use playlist_url directly if it's media)
    std::string master;
    if (cancel_token.load()) return false;
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Downloading master playlist for " + channel_name);
    if (!HttpGetText(playlist_url, master, &cancel_token)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to download master playlist for " + channel_name);
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Master playlist downloaded, size=" + 
               std::to_wstring(master.size()) + L" for " + channel_name);

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
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Using media playlist URL for " + channel_name + 
               L", is_master=" + std::to_wstring(is_master));

    // 2. Create a pipe for stdin redirection to the player with resource manager settings
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Creating pipe with buffer size " + 
               std::to_wstring(pipe_buffer) + L" for " + channel_name);
    
    if (!CreatePipe(&hRead, &hWrite, &sa, pipe_buffer)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create pipe for " + channel_name + 
                   L", Error=" + std::to_wstring(GetLastError()));
        return false;
    }
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe created successfully, hRead=" + 
               std::to_wstring((uintptr_t)hRead) + L", hWrite=" + std::to_wstring((uintptr_t)hWrite) + 
               L" for " + channel_name);

    // Create isolated output/error handles for better process separation
    HANDLE hNullOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE hNullErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Created NULL handles, hNullOut=" + 
               std::to_wstring((uintptr_t)hNullOut) + L", hNullErr=" + std::to_wstring((uintptr_t)hNullErr) + 
               L" for " + channel_name);

    // 3. Launch the player with resource manager isolation
    std::wstring cmd = L"\"" + player_path + L"\" -";
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Player command: " + cmd + L" for " + channel_name);
    
    // Update quota with current settings
    quota.process_priority = process_priority;
    quota.pipe_buffer_size = pipe_buffer;
    
    PROCESS_INFORMATION pi = {};
    if (!StreamProcessUtils::CreateIsolatedProcess(
        cmd, channel_name, quota, &pi, hRead, hNullOut, hNullErr)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
        if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
        return false;
    }
    
    CloseHandle(hRead);
    
    // Assign process to resource manager
    if (!resource_guard.AssignProcess(pi.hProcess, pi.dwProcessId)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to assign process to resource manager for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hWrite);
        if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
        if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
        return false;
    }
    
    // Resume process after job assignment (if it was created suspended)
    if (quota.use_job_object && !StreamProcessUtils::ResumeProcessAfterJobAssignment(pi.hThread, channel_name)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to resume process after job assignment for " + channel_name);
        // Continue anyway as the process might not have been created suspended
    }
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Player process created and assigned to resource manager, PID=" + 
               std::to_wstring(pi.dwProcessId) + L" for " + channel_name);

    // Set the player window title to the channel name (in a separate thread to avoid blocking)
    if (!channel_name.empty()) {
        std::thread([processId = pi.dwProcessId, title = channel_name]() {
            SetPlayerWindowTitle(processId, title);
        }).detach();
    }

    // 4. Download media playlist and buffer segments with resource manager monitoring
    std::set<std::wstring> downloaded;
    std::vector<std::vector<char>> segment_buffer;
    bool started = false;
    int consecutive_errors = 0;
    const int max_consecutive_errors = 30; // ~60 seconds with 2s sleep - more tolerant for live streams
    bool stream_ended = false;

    // Use adaptive polling based on system load
    int base_poll_interval = resource_manager.IsSystemUnderLoad() ? 3000 : 2000;
    int random_jitter = rand() % 1000;
    int poll_interval = base_poll_interval + random_jitter;
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting streaming loop for " + channel_name + 
               L", max_errors=" + std::to_wstring(max_consecutive_errors) + 
               L", poll_interval=" + std::to_wstring(poll_interval));
    
    while (!cancel_token.load() && !stream_ended) {
        // Use resource manager for more robust process health checking
        if (!resource_guard.IsProcessHealthy()) {
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Process health check failed for " + channel_name);
            break;
        }
        
        std::string playlist;
        if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
            consecutive_errors++;
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Playlist download failed " + 
                       std::to_wstring(consecutive_errors) + L"/" + 
                       std::to_wstring(max_consecutive_errors) + L" for " + channel_name);
            if (consecutive_errors > max_consecutive_errors) {
                // Too many consecutive errors - likely stream is down or network issues
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Too many consecutive errors, stopping for " + channel_name);
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
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Stream ended marker found for " + channel_name);
            stream_ended = true;
        }
        
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Found " + std::to_wstring(segments.size()) + 
                   L" segments, ended=" + std::to_wstring(playlist_ended) + L" for " + channel_name);

        size_t new_segments = 0;
        for (auto& seg : segments) {
            if (cancel_token.load()) break;
            if (!resource_guard.IsProcessHealthy()) break;
            if (downloaded.count(seg)) continue;
            downloaded.insert(seg);

            // Download segment (with retry)
            std::wstring seg_url = JoinUrl(media_playlist_url, seg);
            std::vector<char> segment_data;
            int seg_attempts = 0;
            while (seg_attempts < 3 && !HttpGetBinary(seg_url, segment_data, 1, &cancel_token)) {
                if (cancel_token.load()) break;
                if (!resource_guard.IsProcessHealthy()) break;
                seg_attempts++;
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Segment download attempt " + 
                           std::to_wstring(seg_attempts) + L" failed for " + channel_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
            
            if (!resource_guard.IsProcessHealthy()) break;
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
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting to write buffer (" + 
                               std::to_wstring(segment_buffer.size()) + L" segments) to player for " + channel_name);
                    for (auto& buf : segment_buffer) {
                        if (cancel_token.load()) break;
                        if (!resource_guard.IsProcessHealthy()) break;
                        
                        DWORD written = 0;
                        if (!WriteFile(hWrite, buf.data(), (DWORD)buf.size(), &written, nullptr)) {
                            DWORD error = GetLastError();
                            AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe write failed for " + channel_name + 
                                       L", Error=" + std::to_wstring(error));
                            goto streaming_cleanup;
                        }
                        if (written != buf.size()) {
                            AddDebugLog(L"BufferAndPipeStreamToPlayer: Partial write for " + channel_name);
                            break;
                        }
                        // Small delay to prevent overwhelming the player
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if (!resource_guard.IsProcessHealthy()) break;
                    segment_buffer.clear();
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                } else if (started) {
                    if (cancel_token.load()) break;
                    if (!resource_guard.IsProcessHealthy()) break;
                    
                    auto& buf = segment_buffer.front();
                    DWORD written = 0;
                    if (!WriteFile(hWrite, buf.data(), (DWORD)buf.size(), &written, nullptr)) {
                        DWORD error = GetLastError();
                        AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe write failed during streaming for " + channel_name + 
                                   L", Error=" + std::to_wstring(error));
                        goto streaming_cleanup;
                    }
                    if (written != buf.size()) {
                        AddDebugLog(L"BufferAndPipeStreamToPlayer: Partial write during streaming for " + channel_name);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    segment_buffer.erase(segment_buffer.begin());
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                }
            }
        }

        if (cancel_token.load() || !resource_guard.IsProcessHealthy()) break;
        if (segments.empty() && !stream_ended) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval));
            continue;
        }

        // Wait before polling playlist again
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval));
    }

streaming_cleanup:
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup starting for " + channel_name + 
               L", stream_ended=" + std::to_wstring(stream_ended));
    
    // 5. Cleanup: close write pipe, cleanup handles
    CloseHandle(hWrite);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    
    if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
    if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
    
    // Resource guard destructor will handle resource cleanup
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Returning " + std::to_wstring(stream_ended) + L" for " + channel_name);
    return stream_ended;
}