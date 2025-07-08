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
#include <algorithm>
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

// Parse media segment URLs from m3u8 playlist, filtering out segments with unwanted messages
static std::vector<std::wstring> ParseSegments(const std::string& playlist) {
    std::vector<std::wstring> segs;
    std::istringstream ss(playlist);
    std::string line;
    std::string last_metadata;
    bool skip_next_segment = false;
    
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // Store metadata for analysis
            last_metadata = line;
            
            // Check if this metadata indicates a message we want to filter
            std::string line_lower = line;
            std::transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);
            
            if (line_lower.find("preparing stream") != std::string::npos ||
                line_lower.find("commercial break") != std::string::npos) {
                skip_next_segment = true;
            }
            continue;
        }
        
        // This is a segment URL
        if (skip_next_segment) {
            // Skip this segment as it contains filtered content
            skip_next_segment = false;
            continue;
        }
        
        // Check if the segment URL itself contains filtered terms
        std::string line_lower = line;
        std::transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);
        
        if (line_lower.find("preparing") != std::string::npos ||
            line_lower.find("commercial") != std::string::npos ||
            line_lower.find("break") != std::string::npos) {
            // Skip segments with these terms in the URL
            continue;
        }
        
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

    // 2. Create a pipe for stdin redirection to the player
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;

    // 3. Launch the player with stdin redirected
    std::wstring cmd = L"\"" + player_path + L"\" -";
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        nullptr, (LPWSTR)cmd.c_str(),
        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi
    );
    CloseHandle(hRead);
    if (!ok) { CloseHandle(hWrite); return false; }

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
    const int max_consecutive_errors = 12; // ~24 seconds with 2s sleep

    while (!cancel_token.load() && ProcessStillRunning(pi.hProcess)) {
        std::string playlist;
        if (!HttpGetText(media_playlist_url, playlist, &cancel_token)) {
            consecutive_errors++;
            if (consecutive_errors > max_consecutive_errors) break; // too many errors
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        consecutive_errors = 0;
        auto segments = ParseSegments(playlist);

        size_t new_segments = 0;
        for (auto& seg : segments) {
            if (cancel_token.load() || !ProcessStillRunning(pi.hProcess)) break;
            if (downloaded.count(seg)) continue;
            downloaded.insert(seg);

            // Download segment (with retry)
            std::wstring seg_url = JoinUrl(media_playlist_url, seg);
            std::vector<char> segment_data;
            int seg_attempts = 0;
            while (seg_attempts < 3 && !HttpGetBinary(seg_url, segment_data, 1, &cancel_token)) {
                if (cancel_token.load() || !ProcessStillRunning(pi.hProcess)) break;
                seg_attempts++;
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
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
                        if (cancel_token.load() || !ProcessStillRunning(pi.hProcess)) break;
                        DWORD written = 0;
                        WriteFile(hWrite, buf.data(), (DWORD)buf.size(), &written, nullptr);
                    }
                    segment_buffer.clear();
                    // Update chunk count after clearing buffer
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                } else if (started) {
                    if (cancel_token.load() || !ProcessStillRunning(pi.hProcess)) break;
                    auto& buf = segment_buffer.front();
                    DWORD written = 0;
                    WriteFile(hWrite, buf.data(), (DWORD)buf.size(), &written, nullptr);
                    segment_buffer.erase(segment_buffer.begin());
                    // Update chunk count after removing segment
                    if (chunk_count) {
                        *chunk_count = (int)segment_buffer.size();
                    }
                }
            }
        }

        if (cancel_token.load() || !ProcessStillRunning(pi.hProcess)) break;
        if (segments.empty()) { // End of stream or error
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Wait for a few seconds before polling playlist again
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // 5. Cleanup: close write pipe, let player exit
    CloseHandle(hWrite);
    if (ProcessStillRunning(pi.hProcess)) {
        // Ask player to exit
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}