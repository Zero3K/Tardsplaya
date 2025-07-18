#include "ipc_implementations.h"
#include "stream_pipe.h"
#include "tsduck_hls_wrapper.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <set>
#include <sstream>
#include <memory>
#include <algorithm>

// Undefine Windows min/max macros to avoid conflicts with std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#pragma comment(lib, "winhttp.lib")

// Global configuration for IPC method selection
IPCMethod g_current_ipc_method = IPCMethod::ANONYMOUS_PIPES;

// Forward declarations from stream_pipe.cpp
extern void AddDebugLog(const std::wstring& message);
extern bool HttpGetText(const std::wstring& url, std::string& out);
extern std::wstring JoinUrl(const std::wstring& base_url, const std::wstring& relative_url);
extern bool ProcessStillRunning(HANDLE process_handle, const std::wstring& context, DWORD pid);
extern void SetPlayerWindowTitle(DWORD process_id, const std::wstring& channel_name);

// Wrapper function to handle cancel token parameter by ignoring it for now
bool HttpGetTextWithCancel(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token) {
    // For now, ignore the cancel token and use the global function
    // In a full implementation, we would check cancel_token periodically
    if (cancel_token && cancel_token->load()) {
        return false;
    }
    return HttpGetText(url, out);
}

// TSDuck-enhanced segment analyzer for IPC implementations
static std::pair<int, std::chrono::milliseconds> AnalyzePlaylistWithTSDuckIPC(const std::string& playlist, const std::wstring& channel_name) {
    tsduck_hls::PlaylistParser tsduck_parser;
    if (!tsduck_parser.ParsePlaylist(playlist)) {
        // Return conservative values if parsing fails for better stability
        AddDebugLog(L"[TSDUCK-IPC] Failed to parse playlist for " + channel_name + L", using conservative values");
        return std::make_pair(12, std::chrono::milliseconds(6000)); // Increased from 3 to 12 segments
    }
    
    // Get TSDuck's recommendations for optimal buffering
    int optimal_buffer_segments = tsduck_parser.GetOptimalBufferSegments();
    std::chrono::milliseconds playlist_duration = tsduck_parser.GetPlaylistDuration();
    bool has_ads = tsduck_parser.HasAdMarkers();
    
    // When ads are detected, increase buffer size to account for placeholder segments
    // Placeholder segments provide minimal content but consume buffer slots
    if (has_ads) {
        // Increase buffer by 50% when ads are present to compensate for timing disruption
        optimal_buffer_segments = static_cast<int>(optimal_buffer_segments * 1.5);
        
        // Ensure minimum buffer size for ad-heavy content
        optimal_buffer_segments = std::max(optimal_buffer_segments, 10);
        
        AddDebugLog(L"[TSDUCK-IPC] Ad content detected for " + channel_name + 
                   L" - increased buffer recommendation to " + std::to_wstring(optimal_buffer_segments) + 
                   L" segments for better ad handling");
    }
    
    AddDebugLog(L"[TSDUCK-IPC] Analysis for " + channel_name + L": optimal_buffer=" + 
               std::to_wstring(optimal_buffer_segments) + 
               L", playlist_duration=" + std::to_wstring(playlist_duration.count()) + L"ms" +
               L", has_ads=" + std::to_wstring(has_ads) +
               L", live=" + std::to_wstring(tsduck_parser.IsLiveStream()));
    
    return std::make_pair(optimal_buffer_segments, playlist_duration);
}

// Enhanced segment parsing using TSDuck for IPC implementations
static std::vector<std::wstring> ParseSegmentsWithTSDuck(const std::string& playlist, const std::wstring& channel_name) {
    tsduck_hls::PlaylistParser tsduck_parser;
    std::vector<std::wstring> segments;
    
    if (!tsduck_parser.ParsePlaylist(playlist)) {
        AddDebugLog(L"[TSDUCK-IPC] TSDuck parsing failed for " + channel_name + L", falling back to basic parsing");
        
        // Fallback to basic parsing
        std::istringstream ss(playlist);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line[0] != '#' && line.find(".ts") != std::string::npos) {
                segments.push_back(std::wstring(line.begin(), line.end()));
            }
        }
        return segments;
    }
    
    // Use TSDuck's enhanced segment parsing
    auto tsduck_segments = tsduck_parser.GetSegments();
    for (const auto& seg : tsduck_segments) {
        if (!seg.is_ad_segment) { // Skip ad segments using TSDuck's detection
            segments.push_back(seg.url);
        } else {
            AddDebugLog(L"[TSDUCK-IPC] Skipping ad segment for " + channel_name + L": " + seg.url);
        }
    }
    
    AddDebugLog(L"[TSDUCK-IPC] Parsed " + std::to_wstring(segments.size()) + L" segments for " + 
               channel_name + L" (total: " + std::to_wstring(tsduck_segments.size()) + L")");
    
    return segments;
}

// Define WriteFileWithTimeout if not available
BOOL WriteFileWithTimeout(HANDLE handle, const void* buffer, DWORD bytes_to_write, DWORD* bytes_written, DWORD timeout_ms) {
    // For simplicity, use regular WriteFile for now
    return WriteFile(handle, buffer, bytes_to_write, bytes_written, nullptr);
}

// Utility: HTTP GET (returns as binary), with cancellation support
static bool HttpGetBinaryWithCancel(const std::wstring& url, std::vector<char>& out, std::atomic<bool>* cancel_token) {
    for (int attempt = 0; attempt < 3; ++attempt) {
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

// Helper: join relative URL to base
static std::wstring JoinUrl(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}

static std::atomic<int> g_mailslot_counter(0);
static std::atomic<int> g_namedpipe_counter(0);

HANDLE CreateMailSlotBridge(const std::wstring& mailslot_name, HANDLE& stdin_write_handle) {
    // Create a bridge process that reads from MailSlot and writes to stdin
    // For now, we'll create a pipe and simulate the bridge functionality
    
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const DWORD PIPE_BUFFER_SIZE = 1024 * 1024;
    
    if (!CreatePipe(&hRead, &hWrite, &sa, PIPE_BUFFER_SIZE)) {
        return INVALID_HANDLE_VALUE;
    }
    
    stdin_write_handle = hWrite;
    return hRead; // This will be used as the process stdin
}

bool SendVideoSegmentViaMailSlot(HANDLE mailslot_client, const std::vector<char>& segment_data) {
    // For actual MailSlot implementation, we send the entire segment in one message
    // Since individual mailslots can handle large messages up to the size specified when creating
    
    DWORD bytes_written = 0;
    BOOL success = WriteFile(
        mailslot_client,
        segment_data.data(),
        (DWORD)segment_data.size(),
        &bytes_written,
        nullptr
    );
    
    return success && bytes_written == segment_data.size();
}

HANDLE CreateStreamingNamedPipe(const std::wstring& pipe_name, HANDLE& client_handle) {
    // Create named pipe server
    HANDLE server_handle = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1, // Only one instance
        1024 * 1024, // 1MB buffer
        1024 * 1024,
        0,
        nullptr
    );
    
    if (server_handle == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    
    // Create anonymous pipe for child process since named pipes can't be used directly as stdin
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 1024 * 1024)) {
        CloseHandle(server_handle);
        return INVALID_HANDLE_VALUE;
    }
    
    client_handle = hRead; // This will be used as stdin for child process
    
    // Start a bridge thread to copy from named pipe to anonymous pipe
    std::thread* bridge_thread = new std::thread([server_handle, hWrite, pipe_name]() {
        AddDebugLog(L"[NAMEDPIPE-BRIDGE] Bridge thread started for " + pipe_name);
        
        // Wait for connection
        if (ConnectNamedPipe(server_handle, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            AddDebugLog(L"[NAMEDPIPE-BRIDGE] Named pipe connected: " + pipe_name);
            
            // Copy data from named pipe to anonymous pipe
            const size_t BUFFER_SIZE = 64 * 1024; // 64KB buffer
            std::unique_ptr<char[]> buffer = std::make_unique<char[]>(BUFFER_SIZE);
            DWORD bytes_read, bytes_written;
            
            while (true) {
                if (ReadFile(server_handle, buffer.get(), (DWORD)BUFFER_SIZE, &bytes_read, nullptr)) {
                    if (bytes_read > 0) {
                        if (!WriteFile(hWrite, buffer.get(), bytes_read, &bytes_written, nullptr)) {
                            AddDebugLog(L"[NAMEDPIPE-BRIDGE] Failed to write to anonymous pipe");
                            break;
                        }
                    }
                } else {
                    DWORD error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
                        AddDebugLog(L"[NAMEDPIPE-BRIDGE] Named pipe closed");
                        break;
                    }
                }
            }
        }
        
        CloseHandle(hWrite);
        AddDebugLog(L"[NAMEDPIPE-BRIDGE] Bridge thread ended for " + pipe_name);
    });
    bridge_thread->detach();
    delete bridge_thread; // Safe to delete after detach
    
    return server_handle;
}

bool BufferAndMailSlotStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    AddDebugLog(L"[IPC-METHOD] MailSlot implementation starting for " + channel_name + L", URL=" + playlist_url);
    
    // Generate unique MailSlot name
    int mailslot_id = ++g_mailslot_counter;
    std::wstring mailslot_name = L"\\\\.\\mailslot\\tardsplaya_" + std::to_wstring(mailslot_id);
    
    // Create MailSlot server
    HANDLE mailslot_server = CreateMailslotW(
        mailslot_name.c_str(),
        10 * 1024 * 1024, // 10MB max message size - large enough for video segments
        MAILSLOT_WAIT_FOREVER,
        nullptr
    );
    
    if (mailslot_server == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create MailSlot server, error=" + std::to_wstring(error));
        return false;
    }
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Created MailSlot server: " + mailslot_name);
    
    // Create bridge process - for simplicity, we'll use pipes to simulate the bridge
    HANDLE bridge_stdin;
    HANDLE player_stdin = CreateMailSlotBridge(mailslot_name, bridge_stdin);
    
    if (player_stdin == INVALID_HANDLE_VALUE) {
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create MailSlot bridge");
        CloseHandle(mailslot_server);
        return false;
    }
    
    // Create media player process
    std::wstring cmd;
    if (player_path.find(L"mpc-hc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - /new /nofocus";
    } else if (player_path.find(L"vlc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - --intf dummy --no-one-instance";
    } else {
        cmd = L"\"" + player_path + L"\" -";
    }
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = player_stdin;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Launching player with MailSlot bridge: " + cmd);
    
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi
    );
    
    CloseHandle(player_stdin); // Child process now owns it
    
    if (!success) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create process, error=" + std::to_wstring(error));
        CloseHandle(bridge_stdin);
        CloseHandle(mailslot_server);
        return false;
    }
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Process created, PID=" + std::to_wstring(pi.dwProcessId));
    
    // Start bridge thread that reads from MailSlot and writes to pipe
    std::thread bridge_thread([mailslot_server, bridge_stdin, channel_name]() {
        AddDebugLog(L"[BRIDGE] MailSlot bridge thread started for " + channel_name);
        
        // Use heap allocation instead of stack to prevent stack overflow
        const size_t BUFFER_SIZE = 10 * 1024 * 1024; // 10MB buffer
        std::unique_ptr<char[]> buffer = std::make_unique<char[]>(BUFFER_SIZE);
        DWORD bytes_read, bytes_written;
        
        while (true) {
            if (ReadFile(mailslot_server, buffer.get(), (DWORD)BUFFER_SIZE, &bytes_read, nullptr)) {
                if (bytes_read > 0) {
                    AddDebugLog(L"[BRIDGE] Read " + std::to_wstring(bytes_read) + L" bytes from MailSlot");
                    if (!WriteFile(bridge_stdin, buffer.get(), bytes_read, &bytes_written, nullptr)) {
                        AddDebugLog(L"[BRIDGE] Failed to write to bridge pipe");
                        break;
                    }
                    AddDebugLog(L"[BRIDGE] Wrote " + std::to_wstring(bytes_written) + L" bytes to player");
                }
            } else {
                DWORD error = GetLastError();
                if (error != ERROR_SEM_TIMEOUT) {
                    AddDebugLog(L"[BRIDGE] MailSlot read error: " + std::to_wstring(error));
                    break;
                }
            }
        }
        
        CloseHandle(bridge_stdin);
        AddDebugLog(L"[BRIDGE] MailSlot bridge thread ended for " + channel_name);
    });
    bridge_thread.detach();
    
    // Create MailSlot client for sending data
    HANDLE mailslot_client = CreateFileW(
        mailslot_name.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (mailslot_client == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create MailSlot client, error=" + std::to_wstring(error));
        CloseHandle(mailslot_server);
        return false;
    }
    
    // Download and parse master playlist to get media playlist URL
    std::string master;
    if (cancel_token.load()) {
        CloseHandle(mailslot_client);
        CloseHandle(mailslot_server);
        return false;
    }
    
    if (!HttpGetTextWithCancel(playlist_url, master, &cancel_token)) {
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to download master playlist");
        CloseHandle(mailslot_client);
        CloseHandle(mailslot_server);
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
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Using media playlist URL=" + media_playlist_url);
    
    // Set up real streaming infrastructure like the original implementation
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::set<std::wstring> seen_urls;
    std::atomic<bool> download_running(true);
    std::atomic<bool> stream_ended_normally(false);
    std::atomic<bool> urgent_download_needed(false);
    
    const int target_buffer_segments = std::max(buffer_segments, 12); // Increased from 10 to 12 for MailSlot latency
    const int max_buffer_segments = target_buffer_segments * 2;
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Starting real video streaming with MailSlots for " + channel_name);
    
    // Background segment downloader thread - adapted from original implementation
    std::thread download_thread([&]() {
        int consecutive_errors = 0;
        const int max_consecutive_errors = 15;
        
        AddDebugLog(L"[DOWNLOAD-MAILSLOT] Starting download thread for " + channel_name);
        
        while (download_running.load() && !cancel_token.load() && consecutive_errors < max_consecutive_errors) {
            // Download current playlist
            std::string playlist;
            if (!HttpGetTextWithCancel(media_playlist_url, playlist, &cancel_token)) {
                consecutive_errors++;
                AddDebugLog(L"[DOWNLOAD-MAILSLOT] Playlist fetch FAILED for " + channel_name + 
                           L", error " + std::to_wstring(consecutive_errors) + L"/" + 
                           std::to_wstring(max_consecutive_errors));
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            consecutive_errors = 0;
            
            // TSDuck-enhanced analysis for dynamic buffer optimization
            static int tsduck_recommended_buffer = target_buffer_segments;
            static bool first_analysis_done = false;
            
            // Run TSDuck analysis on playlist for optimal buffering
            auto tsduck_analysis = AnalyzePlaylistWithTSDuckIPC(playlist, channel_name);
            int new_tsduck_recommendation = tsduck_analysis.first;
            
            // Only log if recommendation changes or on first run to reduce log noise
            if (!first_analysis_done || new_tsduck_recommendation != tsduck_recommended_buffer) {
                std::wstring analysis_type = first_analysis_done ? L"Updated" : L"Initial";
                first_analysis_done = true;
                
                AddDebugLog(L"[TSDUCK-MAILSLOT] " + analysis_type + 
                           L" buffer recommendation: " + std::to_wstring(new_tsduck_recommendation) + 
                           L" segments (was: " + std::to_wstring(tsduck_recommended_buffer) + 
                           L", original: " + std::to_wstring(target_buffer_segments) + L") for " + channel_name);
            }
            
            tsduck_recommended_buffer = new_tsduck_recommendation;
            
            // Use TSDuck's recommendation for effective buffer size
            int effective_buffer_size = std::max(target_buffer_segments, tsduck_recommended_buffer);
            int effective_max_buffer = std::min(effective_buffer_size * 2, 30); // Cap max at 30
            
            // Check for stream end
            if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
                AddDebugLog(L"[DOWNLOAD-MAILSLOT] Found #EXT-X-ENDLIST - stream ended for " + channel_name);
                stream_ended_normally = true;
                break;
            }
            
            // Parse segments using TSDuck enhanced parsing with ad detection
            std::vector<std::wstring> segments = ParseSegmentsWithTSDuck(playlist, channel_name);
            
            // Download new segments
            int new_segments_downloaded = 0;
            for (auto& seg : segments) {
                if (!download_running.load() || cancel_token.load()) break;
                
                // Skip segments we've already seen
                if (seen_urls.count(seg)) continue;
                
                // Check buffer size before downloading more
                size_t current_buffer_size;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    current_buffer_size = buffer_queue.size();
                }
                
                if (current_buffer_size >= effective_max_buffer && !urgent_download_needed.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                
                seen_urls.insert(seg);
                std::wstring seg_url = JoinUrl(media_playlist_url, seg);
                std::vector<char> seg_data;
                
                // Download segment with retries
                bool download_ok = false;
                for (int retry = 0; retry < 3; ++retry) {
                    if (HttpGetBinaryWithCancel(seg_url, seg_data, &cancel_token)) {
                        download_ok = true;
                        break;
                    }
                    if (!download_running.load() || cancel_token.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
                
                if (download_ok && !seg_data.empty()) {
                    // Add to buffer
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        buffer_queue.push(std::move(seg_data));
                    }
                    new_segments_downloaded++;
                    AddDebugLog(L"[DOWNLOAD-MAILSLOT] Downloaded segment " + std::to_wstring(new_segments_downloaded) + 
                               L", buffer=" + std::to_wstring(current_buffer_size + 1) + L" for " + channel_name);
                } else {
                    AddDebugLog(L"[DOWNLOAD-MAILSLOT] FAILED to download segment for " + channel_name);
                }
            }
            
            // Sleep before next playlist fetch - more responsive for MailSlot streaming
            if (urgent_download_needed.load()) {
                urgent_download_needed.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
                // Reduced from 1500ms to 800ms for better responsiveness with MailSlot latency
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
        }
        
        AddDebugLog(L"[DOWNLOAD-MAILSLOT] Download thread ending for " + channel_name);
    });
    
    // Main feeder thread - sends real video segments via MailSlot at proper playback rate
    std::thread feeder_thread([&]() {
        bool started = false;
        const size_t min_buffer_size = 6; // Increased from 3 to 6 for MailSlot latency
        
        AddDebugLog(L"[FEEDER-MAILSLOT] Starting feeder thread for " + channel_name);
        
        while (download_running.load() || !buffer_queue.empty()) {
            if (cancel_token.load()) break;
            
            size_t buffer_size;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                buffer_size = buffer_queue.size();
            }
            
            // Wait for larger initial buffer before starting to account for MailSlot latency
            if (!started) {
                // Require more buffer segments for MailSlot due to bridge process overhead
                size_t required_buffer = std::max((size_t)target_buffer_segments, min_buffer_size + 2);
                if (buffer_size >= required_buffer) {
                    started = true;
                    AddDebugLog(L"[FEEDER-MAILSLOT] Initial buffer ready (" + 
                               std::to_wstring(buffer_size) + L" segments), starting MailSlot feed for " + channel_name);
                } else {
                    AddDebugLog(L"[FEEDER-MAILSLOT] Waiting for initial buffer (" + std::to_wstring(buffer_size) + L"/" +
                               std::to_wstring(required_buffer) + L") for " + channel_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Get one segment to feed at natural playback rate
            std::vector<char> segment_to_feed;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                
                if (buffer_size == 0) {
                    AddDebugLog(L"[FEEDER-MAILSLOT] *** WARNING: Buffer reached 0 for " + channel_name + L" ***");
                    urgent_download_needed.store(true);
                } else if (buffer_size < min_buffer_size) {
                    AddDebugLog(L"[FEEDER-MAILSLOT] *** WARNING: Low buffer (" + std::to_wstring(buffer_size) + 
                               L") for " + channel_name + L" ***");
                    urgent_download_needed.store(true);
                }
                
                if (!buffer_queue.empty()) {
                    segment_to_feed = std::move(buffer_queue.front());
                    buffer_queue.pop();
                }
            }
            
            if (!segment_to_feed.empty()) {
                // Send single segment via MailSlot
                if (!SendVideoSegmentViaMailSlot(mailslot_client, segment_to_feed)) {
                    AddDebugLog(L"[FEEDER-MAILSLOT] Failed to send segment via MailSlot for " + channel_name);
                    break;
                } else {
                    AddDebugLog(L"[FEEDER-MAILSLOT] Successfully sent " + std::to_wstring(segment_to_feed.size()) + 
                               L" bytes via MailSlot for " + channel_name);
                }
                
                // Wait for natural segment duration (HLS segments are typically ~2 seconds)
                // This prevents buffer starvation by feeding at playback rate instead of maximum speed
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            } else {
                // No segments available, check more frequently when buffer is empty
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        
        AddDebugLog(L"[FEEDER-MAILSLOT] Feeder thread ending for " + channel_name);
    });
    
    // Wait for threads to complete or cancellation
    download_thread.join();
    feeder_thread.join();
    
    // Cleanup
    CloseHandle(mailslot_client);
    CloseHandle(mailslot_server);
    
    AddDebugLog(L"[IPC-METHOD] MailSlot streaming completed for " + channel_name);
    return true;
}

bool BufferAndNamedPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    AddDebugLog(L"[IPC-METHOD] Named Pipe implementation starting for " + channel_name + L", URL=" + playlist_url);
    
    // Generate unique pipe name
    int pipe_id = ++g_namedpipe_counter;
    std::wstring pipe_name = L"\\\\.\\pipe\\tardsplaya_" + std::to_wstring(pipe_id);
    
    // Create named pipe
    HANDLE client_handle;
    HANDLE server_handle = CreateStreamingNamedPipe(pipe_name, client_handle);
    
    if (server_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to create named pipe, error=" + std::to_wstring(error));
        return false;
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Created Named Pipe: " + pipe_name);
    
    // Create media player process
    std::wstring cmd;
    if (player_path.find(L"mpc-hc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - /new /nofocus";
    } else if (player_path.find(L"vlc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - --intf dummy --no-one-instance";
    } else {
        cmd = L"\"" + player_path + L"\" -";
    }
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = client_handle;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Launching player with Named Pipe: " + cmd);
    
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi
    );
    
    CloseHandle(client_handle); // Child process now owns it
    
    if (!success) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to create process, error=" + std::to_wstring(error));
        CloseHandle(server_handle);
        return false;
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Process created, PID=" + std::to_wstring(pi.dwProcessId));
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Named pipe bridge set up");
    
    // Download and parse master playlist to get media playlist URL
    std::string master;
    if (cancel_token.load()) {
        CloseHandle(server_handle);
        return false;
    }
    
    if (!HttpGetTextWithCancel(playlist_url, master, &cancel_token)) {
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to download master playlist");
        CloseHandle(server_handle);
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
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Using media playlist URL=" + media_playlist_url);
    
    // Set up real streaming infrastructure like the original implementation
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::set<std::wstring> seen_urls;
    std::atomic<bool> download_running(true);
    std::atomic<bool> stream_ended_normally(false);
    std::atomic<bool> urgent_download_needed(false);
    
    const int target_buffer_segments = std::max(buffer_segments, 10);
    const int max_buffer_segments = target_buffer_segments * 2;
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Starting real video streaming with Named Pipes for " + channel_name);
    
    // Background segment downloader thread - adapted from original implementation
    std::thread download_thread([&]() {
        int consecutive_errors = 0;
        const int max_consecutive_errors = 15;
        
        AddDebugLog(L"[DOWNLOAD-NAMEDPIPE] Starting download thread for " + channel_name);
        
        while (download_running.load() && !cancel_token.load() && consecutive_errors < max_consecutive_errors) {
            // Download current playlist
            std::string playlist;
            if (!HttpGetTextWithCancel(media_playlist_url, playlist, &cancel_token)) {
                consecutive_errors++;
                AddDebugLog(L"[DOWNLOAD-NAMEDPIPE] Playlist fetch FAILED for " + channel_name + 
                           L", error " + std::to_wstring(consecutive_errors) + L"/" + 
                           std::to_wstring(max_consecutive_errors));
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            consecutive_errors = 0;
            
            // TSDuck-enhanced analysis for dynamic buffer optimization
            static int tsduck_recommended_buffer = target_buffer_segments;
            static bool first_analysis_done = false;
            
            // Run TSDuck analysis on playlist for optimal buffering
            auto tsduck_analysis = AnalyzePlaylistWithTSDuckIPC(playlist, channel_name);
            int new_tsduck_recommendation = tsduck_analysis.first;
            
            // Only log if recommendation changes or on first run to reduce log noise
            if (!first_analysis_done || new_tsduck_recommendation != tsduck_recommended_buffer) {
                std::wstring analysis_type = first_analysis_done ? L"Updated" : L"Initial";
                first_analysis_done = true;
                
                AddDebugLog(L"[TSDUCK-NAMEDPIPE] " + analysis_type + 
                           L" buffer recommendation: " + std::to_wstring(new_tsduck_recommendation) + 
                           L" segments (was: " + std::to_wstring(tsduck_recommended_buffer) + 
                           L", original: " + std::to_wstring(target_buffer_segments) + L") for " + channel_name);
            }
            
            tsduck_recommended_buffer = new_tsduck_recommendation;
            
            // Use TSDuck's recommendation for effective buffer size
            int effective_buffer_size = std::max(target_buffer_segments, tsduck_recommended_buffer);
            int effective_max_buffer = std::min(effective_buffer_size * 2, 30); // Cap max at 30
            
            // Check for stream end
            if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
                AddDebugLog(L"[DOWNLOAD-NAMEDPIPE] Found #EXT-X-ENDLIST - stream ended for " + channel_name);
                stream_ended_normally = true;
                break;
            }
            
            // Parse segments using TSDuck enhanced parsing with ad detection
            std::vector<std::wstring> segments = ParseSegmentsWithTSDuck(playlist, channel_name);
            
            // Download new segments
            int new_segments_downloaded = 0;
            for (auto& seg : segments) {
                if (!download_running.load() || cancel_token.load()) break;
                
                // Skip segments we've already seen
                if (seen_urls.count(seg)) continue;
                
                // Check buffer size before downloading more
                size_t current_buffer_size;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    current_buffer_size = buffer_queue.size();
                }
                
                if (current_buffer_size >= effective_max_buffer && !urgent_download_needed.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                
                seen_urls.insert(seg);
                std::wstring seg_url = JoinUrl(media_playlist_url, seg);
                std::vector<char> seg_data;
                
                // Download segment with retries
                bool download_ok = false;
                for (int retry = 0; retry < 3; ++retry) {
                    if (HttpGetBinaryWithCancel(seg_url, seg_data, &cancel_token)) {
                        download_ok = true;
                        break;
                    }
                    if (!download_running.load() || cancel_token.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
                
                if (download_ok && !seg_data.empty()) {
                    // Add to buffer
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        buffer_queue.push(std::move(seg_data));
                    }
                    new_segments_downloaded++;
                    AddDebugLog(L"[DOWNLOAD-NAMEDPIPE] Downloaded segment " + std::to_wstring(new_segments_downloaded) + 
                               L", buffer=" + std::to_wstring(current_buffer_size + 1) + L" for " + channel_name);
                } else {
                    AddDebugLog(L"[DOWNLOAD-NAMEDPIPE] FAILED to download segment for " + channel_name);
                }
            }
            
            // Sleep before next playlist fetch - more responsive for Named Pipe streaming
            if (urgent_download_needed.load()) {
                urgent_download_needed.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
                // Reduced from 1500ms to 1000ms for better responsiveness with Named Pipe latency
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
        
        AddDebugLog(L"[DOWNLOAD-NAMEDPIPE] Download thread ending for " + channel_name);
    });
    
    // Main feeder thread - sends real video segments via Named Pipe at proper playback rate
    std::thread feeder_thread([&]() {
        bool started = false;
        const size_t min_buffer_size = 5; // Increased from 3 to 5 for Named Pipe latency
        
        AddDebugLog(L"[FEEDER-NAMEDPIPE] Starting feeder thread for " + channel_name);
        
        while (download_running.load() || !buffer_queue.empty()) {
            if (cancel_token.load()) break;
            
            size_t buffer_size;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                buffer_size = buffer_queue.size();
            }
            
            // Wait for larger initial buffer before starting to account for Named Pipe latency
            if (!started) {
                size_t required_buffer = std::max((size_t)target_buffer_segments, min_buffer_size + 1);
                if (buffer_size >= required_buffer) {
                    started = true;
                    AddDebugLog(L"[FEEDER-NAMEDPIPE] Initial buffer ready (" + 
                               std::to_wstring(buffer_size) + L" segments), starting Named Pipe feed for " + channel_name);
                } else {
                    AddDebugLog(L"[FEEDER-NAMEDPIPE] Waiting for initial buffer (" + std::to_wstring(buffer_size) + L"/" +
                               std::to_wstring(required_buffer) + L") for " + channel_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Get one segment to feed at natural playback rate
            std::vector<char> segment_to_feed;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                
                if (buffer_size == 0) {
                    AddDebugLog(L"[FEEDER-NAMEDPIPE] *** WARNING: Buffer reached 0 for " + channel_name + L" ***");
                    urgent_download_needed.store(true);
                } else if (buffer_size < min_buffer_size) {
                    AddDebugLog(L"[FEEDER-NAMEDPIPE] *** WARNING: Low buffer (" + std::to_wstring(buffer_size) + 
                               L") for " + channel_name + L" ***");
                    urgent_download_needed.store(true);
                }
                
                if (!buffer_queue.empty()) {
                    segment_to_feed = std::move(buffer_queue.front());
                    buffer_queue.pop();
                }
            }
            
            if (!segment_to_feed.empty()) {
                // Send single segment via Named Pipe
                DWORD bytes_written;
                if (!WriteFile(server_handle, segment_to_feed.data(), (DWORD)segment_to_feed.size(), &bytes_written, nullptr)) {
                    DWORD error = GetLastError();
                    AddDebugLog(L"[FEEDER-NAMEDPIPE] Failed to write segment via Named Pipe for " + channel_name + 
                               L", error=" + std::to_wstring(error));
                    break;
                } else {
                    AddDebugLog(L"[FEEDER-NAMEDPIPE] Successfully sent " + std::to_wstring(bytes_written) + 
                               L" bytes via Named Pipe for " + channel_name);
                }
                
                // Wait for natural segment duration (HLS segments are typically ~2 seconds)
                // This prevents buffer starvation by feeding at playback rate instead of maximum speed
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            } else {
                // No segments available, check more frequently when buffer is empty
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        
        AddDebugLog(L"[FEEDER-NAMEDPIPE] Feeder thread ending for " + channel_name);
    });
    
    // Wait for threads to complete or cancellation
    download_thread.join();
    feeder_thread.join();
    
    // Cleanup
    CloseHandle(server_handle);
    
    AddDebugLog(L"[IPC-METHOD] Named Pipe streaming completed for " + channel_name);
    return true;
}

bool StreamToPlayerWithIPC(
    IPCMethod method,
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    std::wstring method_name;
    switch (method) {
        case IPCMethod::ANONYMOUS_PIPES: method_name = L"Anonymous Pipes"; break;
        case IPCMethod::MAILSLOTS: method_name = L"MailSlots"; break;
        case IPCMethod::NAMED_PIPES: method_name = L"Named Pipes"; break;
        default: method_name = L"Unknown"; break;
    }
    AddDebugLog(L"[IPC-METHOD] StreamToPlayerWithIPC using " + method_name + L" for " + channel_name);
    
    switch (method) {
        case IPCMethod::MAILSLOTS:
            return BufferAndMailSlotStreamToPlayer(player_path, playlist_url, cancel_token, 
                                                 buffer_segments, channel_name, chunk_count, selected_quality);
        
        case IPCMethod::NAMED_PIPES:
            return BufferAndNamedPipeStreamToPlayer(player_path, playlist_url, cancel_token,
                                                  buffer_segments, channel_name, chunk_count, selected_quality);
        
        case IPCMethod::ANONYMOUS_PIPES:
        default:
            // Use the original function (would need to be declared extern)
            AddDebugLog(L"StreamToPlayerWithIPC: Falling back to anonymous pipes (original implementation)");
            return false; // Placeholder - would call original function
    }
}