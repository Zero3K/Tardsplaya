#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX      // Prevent Windows min/max macros
#endif
#include <windows.h>
#include "builtin_streaming.h"
#include "stream_thread.h"
#include <algorithm>
#include <winhttp.h>
#include <sstream>
#include <vector>
#include <set>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>

// Global built-in player instance
static SimpleBuiltinPlayer* g_builtinPlayer = nullptr;
static std::mutex g_builtinPlayerMutex;

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
                AddDebugLog(L"[SIMPLE_PLAYER] Found SCTE35-OUT marker, entering ad block");
                continue;
            }
            // SCTE-35 ad marker detection (end of ad block)
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                in_scte35_out = false;
                AddDebugLog(L"[SIMPLE_PLAYER] Found SCTE35-IN marker, exiting ad block");
                continue;
            }
            // Discontinuity markers (often used with ads)
            else if (line.find("#EXT-X-DISCONTINUITY") == 0 && in_scte35_out) {
                AddDebugLog(L"[SIMPLE_PLAYER] Skipping discontinuity marker in ad block");
                continue;
            }
            // Stitched ad segments
            else if (line.find("stitched-ad") != std::string::npos) {
                skip_next_segment = true;
                AddDebugLog(L"[SIMPLE_PLAYER] Found stitched-ad marker");
            }
            // EXTINF tags with specific durations that indicate ads (2.001, 2.002 seconds)
            else if (line.find("#EXTINF:2.00") == 0 && 
                     (line.find("2.001") != std::string::npos || 
                      line.find("2.002") != std::string::npos)) {
                skip_next_segment = true;
                AddDebugLog(L"[SIMPLE_PLAYER] Found ad-duration EXTINF marker");
            }
            // DATERANGE markers for stitched ads
            else if (line.find("#EXT-X-DATERANGE:ID=\"stitched-ad") == 0) {
                skip_next_segment = true;
                AddDebugLog(L"[SIMPLE_PLAYER] Found stitched-ad DATERANGE marker");
            }
            // General stitched content detection
            else if (line.find("stitched") != std::string::npos ||
                     line.find("STITCHED") != std::string::npos) {
                skip_next_segment = true;
                AddDebugLog(L"[SIMPLE_PLAYER] Found general stitched content marker");
            }
            // MIDROLL ad markers
            else if (line.find("EXT-X-DATERANGE") != std::string::npos && 
                     (line.find("MIDROLL") != std::string::npos ||
                      line.find("midroll") != std::string::npos)) {
                skip_next_segment = true;
                AddDebugLog(L"[SIMPLE_PLAYER] Found MIDROLL ad marker");
            }
            continue;
        }
        
        // This is a segment URL
        if (skip_next_segment || in_scte35_out) {
            // Skip this segment as it contains ad content
            AddDebugLog(L"[SIMPLE_PLAYER] Skipping ad segment: " + std::wstring(line.begin(), line.end()));
            skip_next_segment = false;
            continue;
        }
        
        // Should be a .ts or .aac segment
        std::wstring wline(line.begin(), line.end());
        segs.push_back(wline);
    }
    return segs;
}

bool BufferAndStreamToBuiltinPlayer(
    HWND hwndStatus,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
) {
    AddDebugLog(L"[SIMPLE_PLAYER] Starting built-in streaming for " + channel_name + L", URL=" + playlist_url);
    
    // Get or create the built-in player instance
    SimpleBuiltinPlayer* player = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_builtinPlayerMutex);
        if (!g_builtinPlayer) {
            g_builtinPlayer = new SimpleBuiltinPlayer();
            if (!g_builtinPlayer->Initialize(hwndStatus)) {
                AddDebugLog(L"[SIMPLE_PLAYER] Failed to initialize built-in player");
                delete g_builtinPlayer;
                g_builtinPlayer = nullptr;
                return false;
            }
        }
        player = g_builtinPlayer;
    }
    
    // Download the master playlist and pick the first media playlist
    std::string master;
    if (cancel_token.load()) return false;
    if (!HttpGetText(playlist_url, master, &cancel_token)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to download master playlist for " + channel_name);
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
    AddDebugLog(L"[SIMPLE_PLAYER] Using media playlist URL=" + media_playlist_url + L" for " + channel_name);

    // Start the stream in the built-in player
    if (!player->StartStream(channel_name)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to start stream in built-in player");
        return false;
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] Stream started in built-in player for " + channel_name);

    // Streaming loop with background download
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::set<std::wstring> seen_urls;
    std::atomic<bool> download_running(true);
    std::atomic<bool> stream_ended_normally(false);
    
    const int target_buffer_segments = std::max(buffer_segments, 5); // Minimum 5 segments
    const int max_buffer_segments = target_buffer_segments * 2; // Don't over-buffer
    
    AddDebugLog(L"[SIMPLE_PLAYER] Target buffer: " + std::to_wstring(target_buffer_segments) + 
               L" segments, max: " + std::to_wstring(max_buffer_segments) + L" for " + channel_name);

    // Background playlist monitor and segment downloader thread
    std::thread download_thread([&]() {
        int consecutive_errors = 0;
        const int max_consecutive_errors = 15; // ~30 seconds (2 sec intervals)
        
        AddDebugLog(L"[SIMPLE_PLAYER] Starting download thread for " + channel_name);
        
        while (true) {
            // Check all exit conditions
            bool download_running_check = download_running.load();
            bool cancel_token_check = cancel_token.load();
            bool player_playing_check = player->IsPlaying();
            bool error_limit_check = consecutive_errors < max_consecutive_errors;
            
            if (!download_running_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: download_running=false for " + channel_name);
                break;
            }
            if (cancel_token_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: cancel_token=true for " + channel_name);
                break;
            }
            if (!player_playing_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: player not playing for " + channel_name);
                break;
            }
            if (!error_limit_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: too many consecutive errors (" + 
                           std::to_wstring(consecutive_errors) + L") for " + channel_name);
                break;
            }
            
            // Download current playlist
            std::string playlist;
            AddDebugLog(L"[SIMPLE_PLAYER] Fetching playlist for " + channel_name);
            if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
                consecutive_errors++;
                AddDebugLog(L"[SIMPLE_PLAYER] Playlist fetch FAILED for " + channel_name + 
                           L", error " + std::to_wstring(consecutive_errors) + L"/" + 
                           std::to_wstring(max_consecutive_errors));
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            consecutive_errors = 0;
            AddDebugLog(L"[SIMPLE_PLAYER] Playlist fetch SUCCESS for " + channel_name + 
                       L", size=" + std::to_wstring(playlist.size()) + L" bytes");

            // Check for stream end
            if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
                AddDebugLog(L"[SIMPLE_PLAYER] Found #EXT-X-ENDLIST - stream actually ended for " + channel_name);
                stream_ended_normally = true;
                break;
            }

            auto segments = ParseSegments(playlist);
            AddDebugLog(L"[SIMPLE_PLAYER] Parsed " + std::to_wstring(segments.size()) + 
                       L" segments from playlist for " + channel_name);
            
            // Download new segments
            int new_segments_downloaded = 0;
            for (auto& seg : segments) {
                if (!download_running || cancel_token.load()) {
                    AddDebugLog(L"[SIMPLE_PLAYER] Breaking segment loop - download_running=" + 
                               std::to_wstring(download_running.load()) + L", cancel=" + 
                               std::to_wstring(cancel_token.load()) + L" for " + channel_name);
                    break;
                }
                
                if (!player->IsPlaying()) {
                    AddDebugLog(L"[SIMPLE_PLAYER] Breaking segment loop - player stopped for " + channel_name);
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
                    AddDebugLog(L"[SIMPLE_PLAYER] Buffer full (" + std::to_wstring(current_buffer_size) + 
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
                    AddDebugLog(L"[SIMPLE_PLAYER] Downloaded segment " + std::to_wstring(new_segments_downloaded) + 
                               L", buffer=" + std::to_wstring(current_buffer_size + 1) + L" for " + channel_name);
                } else {
                    AddDebugLog(L"[SIMPLE_PLAYER] FAILED to download segment after retries for " + channel_name);
                }
            }
            
            AddDebugLog(L"[SIMPLE_PLAYER] Segment batch complete - downloaded " + std::to_wstring(new_segments_downloaded) + 
                       L" new segments for " + channel_name);
            
            // Poll playlist more frequently for live streams
            AddDebugLog(L"[SIMPLE_PLAYER] Sleeping 1.5s before next playlist fetch for " + channel_name);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        
        AddDebugLog(L"[SIMPLE_PLAYER] *** DOWNLOAD THREAD ENDING *** for " + channel_name);
    });

    // Main buffer feeding thread - feeds data to built-in player
    std::thread feeder_thread([&]() {
        bool started = false;
        AddDebugLog(L"[SIMPLE_PLAYER] Starting feeder thread for " + channel_name);
        
        while (true) {
            // Check all exit conditions
            bool cancel_token_check = cancel_token.load();
            bool player_playing_check = player->IsPlaying();
            bool data_available_check = (download_running.load() || !buffer_queue.empty());
            
            if (cancel_token_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: cancel_token=true for " + channel_name);
                break;
            }
            if (!player_playing_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: player not playing for " + channel_name);
                break;
            }
            if (!data_available_check) {
                AddDebugLog(L"[SIMPLE_PLAYER] Exit condition: no more data available (download stopped and buffer empty) for " + channel_name);
                break;
            }
            
            size_t buffer_size;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                buffer_size = buffer_queue.size();
            }
            
            // Wait for initial buffer before starting
            if (!started) {
                if (buffer_size >= target_buffer_segments) {
                    started = true;
                    AddDebugLog(L"[SIMPLE_PLAYER] Initial buffer ready (" + 
                               std::to_wstring(buffer_size) + L" segments), starting feed for " + channel_name);
                } else {
                    AddDebugLog(L"[SIMPLE_PLAYER] Waiting for initial buffer (" + std::to_wstring(buffer_size) + L"/" +
                               std::to_wstring(target_buffer_segments) + L") for " + channel_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Feed segment to built-in player
            std::vector<char> segment_data;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!buffer_queue.empty()) {
                    segment_data = std::move(buffer_queue.front());
                    buffer_queue.pop();
                }
            }
            
            if (!segment_data.empty()) {
                if (player->FeedData(segment_data.data(), segment_data.size())) {
                    size_t current_buffer = buffer_size - 1;
                    AddDebugLog(L"[SIMPLE_PLAYER] Fed segment to built-in player, local_buffer=" + 
                               std::to_wstring(current_buffer) + L" for " + channel_name);
                    
                    if (chunk_count) {
                        *chunk_count = (int)current_buffer;
                    }
                    
                    // Shorter wait when actively feeding
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } else {
                    AddDebugLog(L"[SIMPLE_PLAYER] Failed to feed data to built-in player for " + channel_name);
                    break;
                }
            } else {
                // No segments available, wait
                AddDebugLog(L"[SIMPLE_PLAYER] No segments available, waiting... (download_running=" + 
                           std::to_wstring(download_running.load()) + L") for " + channel_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        
        AddDebugLog(L"[SIMPLE_PLAYER] *** FEEDER THREAD ENDING *** for " + channel_name);
    });

    // Wait for download and feeder threads to complete
    download_thread.join();
    download_running = false;
    feeder_thread.join();

    AddDebugLog(L"[SIMPLE_PLAYER] Cleanup starting for " + channel_name + 
               L", cancel=" + std::to_wstring(cancel_token.load()) + 
               L", player_playing=" + std::to_wstring(player->IsPlaying()) +
               L", stream_ended_normally=" + std::to_wstring(stream_ended_normally.load()));

    // Stop the stream in the built-in player
    player->StopStream();
    
    bool normal_end = stream_ended_normally.load();
    bool user_cancel = cancel_token.load();
    
    AddDebugLog(L"[SIMPLE_PLAYER] Cleanup complete for " + channel_name);
    AddDebugLog(L"[SIMPLE_PLAYER] Exit reason: normal_end=" + std::to_wstring(normal_end) + 
               L", user_cancel=" + std::to_wstring(user_cancel) + L" for " + channel_name);
    
    return normal_end || user_cancel;
}

bool InitializeBuiltinPlayerSystem() {
    AddDebugLog(L"[SIMPLE_PLAYER] Initializing simple built-in player system");
    // No complex initialization needed for simple player
    return true;
}

void ShutdownBuiltinPlayerSystem() {
    AddDebugLog(L"[SIMPLE_PLAYER] Shutting down simple built-in player system");
    
    std::lock_guard<std::mutex> lock(g_builtinPlayerMutex);
    if (g_builtinPlayer) {
        delete g_builtinPlayer;
        g_builtinPlayer = nullptr;
    }
}