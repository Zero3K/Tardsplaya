#define NOMINMAX  // Prevent Windows headers from defining min/max macros
#include "stream_pipe.h"
#include "stream_thread.h"
#include "stream_resource_manager.h"
#include "stream_socket.h"
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
#include <algorithm>
#include "tlsclient/tlsclient.h"

// Additional safeguard: Undefine any min/max macros that may still be defined
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Forward declarations
class StreamResourceGuard;

// Socket-based streaming with flow control for multi-stream scenarios
static bool WriteBufferToSocketWithFlowControl(
    StreamSocket& stream_socket,
    const void* buffer, 
    size_t bufferSize, 
    const std::wstring& streamName,
    std::atomic<bool>& cancel_token) {
    
    AddDebugLog(L"WriteBufferToSocketWithFlowControl: Starting write for " + streamName + 
               L", size=" + std::to_wstring(bufferSize));
    
    if (!stream_socket.IsClientConnected()) {
        AddDebugLog(L"WriteBufferToSocketWithFlowControl: Client not connected for " + streamName);
        return false;
    }
    
    bool success = stream_socket.WriteData(buffer, bufferSize, cancel_token);
    
    if (success) {
        AddDebugLog(L"WriteBufferToSocketWithFlowControl: Successfully wrote " + 
                   std::to_wstring(bufferSize) + L" bytes for " + streamName);
    } else {
        AddDebugLog(L"WriteBufferToSocketWithFlowControl: Failed to write data for " + streamName);
    }
    
    return success;
}

// Robust pipe writing with flow control for multi-stream scenarios
static bool WriteBufferToPipeWithFlowControl(
    HANDLE hPipe, 
    const void* buffer, 
    size_t bufferSize, 
    const std::wstring& streamName,
    std::atomic<bool>& cancel_token,
    StreamResourceGuard& resource_guard) {
    
    const char* data = static_cast<const char*>(buffer);
    size_t totalWritten = 0;
    const size_t CHUNK_SIZE = 32768; // 32KB chunks for better flow control
    const int MAX_FLOW_CONTROL_RETRIES = 30; // High tolerance for multi-stream pipe recovery scenarios
    const int MAX_FATAL_ERROR_RETRIES = 3;
    
    AddDebugLog(L"WriteBufferToPipeWithFlowControl: Starting write for " + streamName + 
               L", size=" + std::to_wstring(bufferSize));
    
    while (totalWritten < bufferSize) {
        if (cancel_token.load()) {
            AddDebugLog(L"WriteBufferToPipeWithFlowControl: Cancelled for " + streamName);
            return false;
        }
        
        if (!resource_guard.IsProcessHealthy()) {
            AddDebugLog(L"WriteBufferToPipeWithFlowControl: Process unhealthy for " + streamName);
            return false;
        }
        
        // Calculate chunk size - use smaller chunks when system is under load
        size_t remainingBytes = bufferSize - totalWritten;
        size_t chunkSize = std::min(remainingBytes, CHUNK_SIZE);
        
        // Adaptive chunk sizing based on system load and stream count
        auto& resource_manager = StreamResourceManager::getInstance();
        if (resource_manager.IsSystemUnderLoad() || resource_manager.GetActiveStreamCount() > 2) {
            chunkSize = std::min(chunkSize, static_cast<size_t>(16384)); // Use 16KB chunks under load
        }
        
        DWORD written = 0;
        bool writeSuccess = false;
        int flowControlRetries = 0;
        int fatalErrorRetries = 0;
        
        // Attempt to write the chunk with proper error handling
        while (!writeSuccess && flowControlRetries < MAX_FLOW_CONTROL_RETRIES) {
            if (cancel_token.load() || !resource_guard.IsProcessHealthy()) {
                return false;
            }
            
            if (WriteFile(hPipe, data + totalWritten, static_cast<DWORD>(chunkSize), &written, nullptr)) {
                if (written > 0) {
                    writeSuccess = true;
                    totalWritten += written;
                    
                    AddDebugLog(L"WriteBufferToPipeWithFlowControl: Wrote " + std::to_wstring(written) + 
                               L" bytes for " + streamName + L" (total: " + std::to_wstring(totalWritten) + 
                               L"/" + std::to_wstring(bufferSize) + L")");
                    
                    // Log recovery success if we had previous failures
                    if (flowControlRetries > 0 || fatalErrorRetries > 0) {
                        AddDebugLog(L"WriteBufferToPipeWithFlowControl: Recovered after " + 
                                   std::to_wstring(flowControlRetries) + L" flow control retries and " + 
                                   std::to_wstring(fatalErrorRetries) + L" fatal error retries for " + streamName);
                    }
                    
                    // If partial write, adjust for next iteration
                    if (written < chunkSize) {
                        break; // Continue with remaining data in next iteration
                    }
                } else {
                    // WriteFile succeeded but wrote 0 bytes - treat as flow control issue
                    flowControlRetries++;
                    AddDebugLog(L"WriteBufferToPipeWithFlowControl: Zero bytes written, flow control retry " + 
                               std::to_wstring(flowControlRetries) + L" for " + streamName);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            } else {
                DWORD error = GetLastError();
                AddDebugLog(L"WriteBufferToPipeWithFlowControl: WriteFile failed for " + streamName + 
                           L", Error=" + std::to_wstring(error) + L", chunk size=" + std::to_wstring(chunkSize));
                
                // Classify error types - distinguish between temporary and permanent pipe breaks
                if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE) {
                    // Check if this is a temporary pipe break or permanent process death
                    bool isProcessActuallyDead = !resource_guard.IsProcessHealthy();
                    
                    if (isProcessActuallyDead) {
                        // Process is actually dead - this is a fatal error
                        AddDebugLog(L"WriteBufferToPipeWithFlowControl: Process confirmed dead for " + streamName + 
                                   L", Error=" + std::to_wstring(error));
                        return false;
                    } else {
                        // Process is alive but pipe is temporarily broken - treat as flow control issue
                        // This can happen in multi-stream scenarios due to resource pressure
                        flowControlRetries++;
                        AddDebugLog(L"WriteBufferToPipeWithFlowControl: Temporary pipe break (process alive), retry " + 
                                   std::to_wstring(flowControlRetries) + L" for " + streamName + 
                                   L", Error=" + std::to_wstring(error));
                        
                        // Use adaptive delay for pipe recovery - longer delays for broken pipes
                        DWORD delay = 500 + (flowControlRetries * 200); // 500ms, 700ms, 900ms, etc.
                        if (resource_manager.GetActiveStreamCount() > 2) {
                            delay = delay + (resource_manager.GetActiveStreamCount() * 100); // Additional delay per stream
                        }
                        
                        AddDebugLog(L"WriteBufferToPipeWithFlowControl: Pipe recovery delay " + 
                                   std::to_wstring(delay) + L"ms for " + streamName);
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                        
                        // Significantly reduce chunk size for recovery attempts to avoid overwhelming the player
                        chunkSize = std::max(chunkSize / 8, static_cast<size_t>(512)); // Min 512 bytes for recovery
                        
                        // For pipe breaks, also check process health again after the delay
                        if (!resource_guard.IsProcessHealthy()) {
                            AddDebugLog(L"WriteBufferToPipeWithFlowControl: Process died during pipe recovery for " + streamName);
                            return false;
                        }
                    }
                    
                } else if (error == ERROR_INSUFFICIENT_BUFFER || error == ERROR_NOT_READY || 
                          error == ERROR_BUSY || error == ERROR_NOT_ENOUGH_MEMORY) {
                    // Flow control errors - pipe buffer is full or player is busy
                    flowControlRetries++;
                    AddDebugLog(L"WriteBufferToPipeWithFlowControl: Flow control error, retry " + 
                               std::to_wstring(flowControlRetries) + L" for " + streamName + 
                               L", Error=" + std::to_wstring(error));
                    
                    // Adaptive delay based on system load and stream count
                    DWORD delay = 100 + (flowControlRetries * 50); // 100ms, 150ms, 200ms, etc.
                    if (resource_manager.IsSystemUnderLoad()) {
                        delay *= 2; // Longer delays under load
                    }
                    if (resource_manager.GetActiveStreamCount() > 2) {
                        delay += (resource_manager.GetActiveStreamCount() * 25); // Additional 25ms per stream
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    
                    // Reduce chunk size for better flow control
                    chunkSize = std::max(chunkSize / 2, static_cast<size_t>(2048)); // Min 2KB chunks
                    
                } else {
                    // Unknown error - treat as potential fatal error
                    fatalErrorRetries++;
                    if (fatalErrorRetries >= MAX_FATAL_ERROR_RETRIES) {
                        AddDebugLog(L"WriteBufferToPipeWithFlowControl: Unknown error after " + 
                                   std::to_wstring(fatalErrorRetries) + L" retries for " + streamName + 
                                   L", Error=" + std::to_wstring(error));
                        return false;
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        }
        
        if (!writeSuccess) {
            AddDebugLog(L"WriteBufferToPipeWithFlowControl: Failed to write chunk after all retries for " + streamName);
            return false;
        }
        
        // Small delay between chunks to prevent overwhelming the player
        if (totalWritten < bufferSize) {
            DWORD delay = 5; // Base delay of 5ms
            if (resource_manager.GetActiveStreamCount() > 2) {
                delay *= resource_manager.GetActiveStreamCount(); // Longer delays with more streams
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    
    AddDebugLog(L"WriteBufferToPipeWithFlowControl: Successfully wrote " + std::to_wstring(totalWritten) + 
               L" bytes for " + streamName);
    return true;
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

// New socket-based implementation of BufferAndPipeStreamToPlayer
// This replaces the pipe-based version to fix multi-stream issues

bool BufferAndPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
) {
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting socket-based streaming for " + channel_name + 
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
    DWORD process_priority = resource_manager.GetRecommendedProcessPriority();
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Resource settings for " + channel_name + 
               L", delay=" + std::to_wstring(start_delay) + 
               L", priority=" + std::to_wstring(process_priority));
    
    // Apply recommended start delay to spread out resource usage
    if (start_delay > 0) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Applying start delay " + 
                   std::to_wstring(start_delay) + L"ms for " + channel_name);
        std::this_thread::sleep_for(std::chrono::milliseconds(start_delay));
    }
    
    // 1. Download the master playlist and pick the first media playlist
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

    // 2. Create a local TCP socket for streaming instead of pipes
    StreamSocket stream_socket;
    if (!stream_socket.Initialize()) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to initialize socket for " + channel_name);
        return false;
    }
    
    if (!stream_socket.StartListening()) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to start listening on socket for " + channel_name);
        return false;
    }
    
    std::wstring stream_url = stream_socket.GetStreamUrl();
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Socket listening on " + stream_url + L" for " + channel_name);

    // 3. Launch the player with the socket URL instead of stdin
    std::wstring cmd = L"\"" + player_path + L"\" \"" + stream_url + L"\"";
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Player command: " + cmd + L" for " + channel_name);
    
    // Update quota with current settings
    quota.process_priority = process_priority;
    
    // Create NULL handles for output/error redirection
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hNullOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE hNullErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    
    PROCESS_INFORMATION pi = {};
    if (!StreamProcessUtils::CreateIsolatedProcess(
        cmd, channel_name, quota, &pi, NULL, hNullOut, hNullErr)) {
        if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
        if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
        return false;
    }
    
    // Assign process to resource manager
    if (!resource_guard.AssignProcess(pi.hProcess, pi.dwProcessId)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to assign process to resource manager for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
        if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
        return false;
    }
    
    // Resume process if it was created suspended
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to resume process after job assignment for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
        if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
        return false;
    }
    
    CloseHandle(pi.hThread);
    if (hNullOut != INVALID_HANDLE_VALUE) CloseHandle(hNullOut);
    if (hNullErr != INVALID_HANDLE_VALUE) CloseHandle(hNullErr);
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Player process created and assigned to resource manager, PID=" + 
               std::to_wstring(pi.dwProcessId) + L" for " + channel_name);

    // 4. Wait for player to connect to our socket
    if (!stream_socket.AcceptConnection(cancel_token)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to accept connection for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        return false;
    }
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Player connected, starting streaming for " + channel_name);

    // 5. Start streaming loop
    std::set<std::wstring> sent_segments;
    std::vector<char> segment_buf;
    int consecutive_errors = 0;
    const int max_consecutive_errors = 30; // Increased tolerance for network issues
    bool stream_ended = false;
    
    // Set window title
    std::thread title_thread([pi, channel_name]() {
        SetPlayerWindowTitle(pi.dwProcessId, channel_name);
    });
    title_thread.detach();
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting streaming loop for " + channel_name + 
               L", MaxErrors=" + std::to_wstring(max_consecutive_errors));
    
    while (!cancel_token.load() && stream_socket.IsClientConnected()) {
        // Check if player process is still running
        if (!resource_guard.IsProcessHealthy()) {
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Process health check failed for " + channel_name);
            break;
        }
        
        // Download the media playlist
        std::string media;
        if (!HttpGetText(media_playlist_url, media, &cancel_token)) {
            consecutive_errors++;
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Playlist download failed " + 
                       std::to_wstring(consecutive_errors) + L"/" + 
                       std::to_wstring(max_consecutive_errors) + L" for " + channel_name);
            
            if (consecutive_errors >= max_consecutive_errors) {
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Too many consecutive errors, stopping for " + channel_name);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }
        
        // Reset error count on successful download
        consecutive_errors = 0;
        
        // Parse segments and check for end marker
        bool ended = false;
        std::vector<std::wstring> segments = ParseSegments(media, &ended);
        
        if (ended) {
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Stream ended marker found for " + channel_name);
            stream_ended = true;
            break;
        }
        
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Found " + std::to_wstring(segments.size()) + 
                   L" segments for " + channel_name);
        
        // Download and send new segments
        bool sent_any = false;
        for (const auto& seg : segments) {
            if (cancel_token.load() || !stream_socket.IsClientConnected()) break;
            
            if (sent_segments.find(seg) != sent_segments.end()) {
                continue; // Already sent this segment
            }
            
            // Download segment
            if (!HttpGetBinary(seg, segment_buf, 3, &cancel_token)) {
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Segment download failed for " + channel_name);
                continue;
            }
            
            // Write to socket
            if (segment_buf.size() > 0) {
                if (WriteBufferToSocketWithFlowControl(stream_socket, segment_buf.data(), segment_buf.size(), channel_name, cancel_token)) {
                    sent_segments.insert(seg);
                    sent_any = true;
                    
                    if (chunk_count) {
                        chunk_count->fetch_add(1);
                    }
                    
                    AddDebugLog(L"WriteBufferToSocketWithFlowControl: Wrote " + std::to_wstring(segment_buf.size()) + 
                               L" bytes for segment " + seg.substr(seg.find_last_of(L'/') + 1) + L" (" + channel_name + L")");
                } else {
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to write segment to socket for " + channel_name);
                    // Don't break immediately - the client might reconnect
                }
            }
        }
        
        // Cleanup old segments to prevent memory bloat
        if (sent_segments.size() > 20) {
            sent_segments.clear();
        }
        
        // Wait before next playlist refresh
        std::this_thread::sleep_for(std::chrono::milliseconds(sent_any ? 1000 : 5000));
    }
    
    // Cleanup
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup starting for " + channel_name + 
               L", stream_ended=" + std::to_wstring(stream_ended) + 
               L", cancel_token=" + std::to_wstring(cancel_token.load()));
    
    stream_socket.Close();
    
    // Wait a moment for graceful termination, then force kill if needed
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Terminating process after timeout for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
    }
    
    CloseHandle(pi.hProcess);
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Returning " + std::to_wstring(stream_ended) + L" for " + channel_name);
    return stream_ended || cancel_token.load();
}