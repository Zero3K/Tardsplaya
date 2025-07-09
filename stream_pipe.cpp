#include "stream_pipe.h"
#include "stream_thread.h"
#include "stream_memory_map.h"
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
static bool ProcessStillRunning(HANDLE hProcess) {
    DWORD code = 0;
    if (GetExitCodeProcess(hProcess, &code))
        return code == STILL_ACTIVE;
    return false;
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
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting with memory-mapped buffering for " + channel_name + L", URL=" + playlist_url);
    
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

    // 2. Create memory-mapped buffer for internal buffering (64MB buffer)
    StreamMemoryMap memory_buffer;
    if (!memory_buffer.CreateAsWriter(channel_name, 64 * 1024 * 1024)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create memory buffer for " + channel_name);
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Created 64MB memory buffer for " + channel_name);

    // 3. Create pipe for media player
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 128*1024)) { // 128KB pipe buffer
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create pipe for " + channel_name);
        memory_buffer.Close();
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Created pipe for " + channel_name);

    // 4. Create media player process
    std::wstring cmd = L"\"" + player_path + L"\" -";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {};
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Launching player: " + cmd + L" for " + channel_name);
    
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi
    );

    if (!success) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to create process for " + channel_name + 
                   L", Error=" + std::to_wstring(GetLastError()));
        CloseHandle(hRead); CloseHandle(hWrite);
        memory_buffer.Close();
        return false;
    }
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Process created successfully for " + channel_name + 
               L", PID=" + std::to_wstring(pi.dwProcessId));

    CloseHandle(hRead); // Media player process now owns this

    // 5. Start thread to maintain player window title with channel name
    std::thread title_thread([pi, channel_name]() {
        SetPlayerWindowTitle(pi.dwProcessId, channel_name);
    });
    title_thread.detach();

    // 6. Start background thread to read from memory buffer and write to pipe
    std::atomic<bool> pipe_writer_active = true;
    std::atomic<bool> stream_ended = false;
    
    std::thread pipe_writer([&memory_buffer, hWrite, &pipe_writer_active, &cancel_token, &stream_ended, channel_name, &pi]() {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe writer thread started for " + channel_name);
        
        const size_t CHUNK_SIZE = 32768; // 32KB chunks for smooth playback
        std::vector<char> chunk_buffer(CHUNK_SIZE);
        
        while (pipe_writer_active.load() && !cancel_token.load() && ProcessStillRunning(pi.hProcess)) {
            size_t bytes_read = memory_buffer.ReadData(chunk_buffer.data(), CHUNK_SIZE);
            
            if (bytes_read > 0) {
                DWORD written = 0;
                BOOL write_ok = WriteFile(hWrite, chunk_buffer.data(), static_cast<DWORD>(bytes_read), &written, nullptr);
                
                if (!write_ok) {
                    DWORD error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
                        AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe closed by player for " + channel_name);
                        break;
                    } else {
                        AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe write failed with error " + 
                                   std::to_wstring(error) + L" for " + channel_name);
                        // Try a few retries with delays for temporary issues
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                } else {
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Wrote " + std::to_wstring(written) + 
                               L" bytes to pipe for " + channel_name);
                }
            } else {
                // No data available, check if stream ended
                if (memory_buffer.IsStreamEnded()) {
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Stream ended, pipe writer finishing for " + channel_name);
                    stream_ended = true;
                    break;
                }
                // Brief pause when no data available
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Pipe writer thread finished for " + channel_name);
    });

    // 7. Download segments and write to memory buffer
    std::set<std::wstring> downloaded;
    std::vector<std::vector<char>> segment_buffer;
    bool started = false;
    int consecutive_errors = 0;
    const int max_consecutive_errors = 30; // ~60 seconds of errors before giving up
    bool stream_ended_normally = false;
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting segment download loop for " + channel_name);
    
    while (!cancel_token.load() && ProcessStillRunning(pi.hProcess) && !stream_ended.load()) {
        std::string playlist;
        if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
            consecutive_errors++;
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to download playlist, errors=" + 
                       std::to_wstring(consecutive_errors) + L"/" + 
                       std::to_wstring(max_consecutive_errors) + L" for " + channel_name);
            if (consecutive_errors > max_consecutive_errors) break;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        consecutive_errors = 0;

        // Check for HLS stream end marker
        if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
            AddDebugLog(L"BufferAndPipeStreamToPlayer: Found stream end marker for " + channel_name);
            stream_ended_normally = true;
            break;
        }

        auto segments = ParseSegments(playlist);
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Found " + std::to_wstring(segments.size()) + 
                   L" segments for " + channel_name);

        size_t new_segments = 0;
        for (auto& seg : segments) {
            if (cancel_token.load() || !ProcessStillRunning(pi.hProcess) || stream_ended.load()) break;
            if (downloaded.count(seg)) continue;
            downloaded.insert(seg);

            std::wstring seg_url = JoinUrl(media_playlist_url, seg);
            std::vector<char> seg_data;
            bool download_ok = false;
            for (int retry = 0; retry < 3; ++retry) {
                if (HttpGetBinary(seg_url, seg_data, 1, &cancel_token)) {
                    download_ok = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (download_ok && !seg_data.empty()) {
                segment_buffer.push_back(std::move(seg_data));
                new_segments++;
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Downloaded segment " + std::to_wstring(new_segments) + 
                           L", buffer_size=" + std::to_wstring(segment_buffer.size()) + L" for " + channel_name);
                if (chunk_count) {
                    *chunk_count = (int)segment_buffer.size();
                }

                // Once buffer is filled, start writing to memory buffer
                if (!started && (int)segment_buffer.size() >= buffer_segments) {
                    started = true;
                    AddDebugLog(L"BufferAndPipeStreamToPlayer: Starting to write buffered segments (" + 
                               std::to_wstring(segment_buffer.size()) + L") to memory buffer for " + channel_name);
                    for (auto& buf : segment_buffer) {
                        if (cancel_token.load() || !ProcessStillRunning(pi.hProcess) || stream_ended.load()) break;
                        if (!memory_buffer.WriteData(buf.data(), buf.size(), cancel_token)) {
                            AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to write to memory buffer for " + channel_name);
                            goto cleanup;
                        }
                    }
                    segment_buffer.clear();
                    if (chunk_count) {
                        *chunk_count = 0;
                    }
                } else if (started && !segment_buffer.empty()) {
                    auto& buf = segment_buffer.front();
                    if (!memory_buffer.WriteData(buf.data(), buf.size(), cancel_token)) {
                        AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to write streaming data to memory buffer for " + channel_name);
                        goto cleanup;
                    }
                    segment_buffer.erase(segment_buffer.begin());
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                }
            } else {
                AddDebugLog(L"BufferAndPipeStreamToPlayer: Failed to download segment after retries for " + channel_name);
            }
        }

        if (cancel_token.load() || !ProcessStillRunning(pi.hProcess) || stream_ended.load()) break;
        if (segments.empty()) {
            AddDebugLog(L"BufferAndPipeStreamToPlayer: No segments found, waiting for " + channel_name);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Wait before polling playlist again
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

cleanup:

    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup starting for " + channel_name + 
               L", cancel=" + std::to_wstring(cancel_token.load()) + 
               L", process_running=" + std::to_wstring(ProcessStillRunning(pi.hProcess)) +
               L", stream_ended_normally=" + std::to_wstring(stream_ended_normally));

    // Signal stream end in memory buffer
    memory_buffer.SignalStreamEnd();
    
    // Wait for pipe writer to finish
    pipe_writer_active = false;
    if (pipe_writer.joinable()) {
        pipe_writer.join();
    }

    // Cleanup handles
    CloseHandle(hWrite);
    if (ProcessStillRunning(pi.hProcess)) {
        AddDebugLog(L"BufferAndPipeStreamToPlayer: Terminating player process for " + channel_name);
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    
    AddDebugLog(L"BufferAndPipeStreamToPlayer: Cleanup complete for " + channel_name);
    return stream_ended_normally || cancel_token.load();
}