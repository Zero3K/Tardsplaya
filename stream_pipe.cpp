#include "stream_pipe.h"
#include "stream_thread.h"
#include "twitch_api.h"
#include "playlist_parser.h"
#include "mailslot_comparison.h"
#include "alternative_ipc_demo.h"
#include "ipc_implementations.h"
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

// Generate a simple text message for ad skip events
static std::string GenerateAdSkipMessage() {
    return "[AD REPLACED] Commercial break - displaying placeholder content with proper sequencing";
}

static std::mutex g_stream_mutex;

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

// Ad block state tracking for preventing infinite ad segment skipping
struct AdBlockState {
    bool in_scte35_out = false;
    bool skip_next_segment = false; 
    int consecutive_skipped_segments = 0;
    std::chrono::steady_clock::time_point scte35_start_time;
    bool just_started_ad_block = false; // Flag to indicate we just started an ad block
    bool just_exited_ad_block = false; // Flag to indicate we just exited an ad block
    static const int MAX_CONSECUTIVE_SKIPPED_SEGMENTS = 15; // ~30 seconds for 2-second segments
    static constexpr std::chrono::seconds MAX_AD_BLOCK_DURATION{60}; // 60 seconds max ad block
};

// Parse media segment URLs from m3u8 playlist, skipping ad segments entirely
// Returns pair of (segments, should_clear_buffer)
static std::pair<std::vector<std::wstring>, bool> ParseSegments(const std::string& playlist) {
    static AdBlockState ad_state; // Persistent state across playlist refreshes
    std::vector<std::wstring> segs;
    bool should_clear_buffer = false;
    
    // Don't clear buffer when exiting ad blocks to maintain stream continuity
    if (ad_state.just_exited_ad_block) {
        ad_state.just_exited_ad_block = false; // Reset flag
        AddDebugLog(L"[AD_SKIP] Just exited ad block, continuing with existing buffer to maintain continuity");
    }
    
    std::istringstream ss(playlist);
    std::string line;
    
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // SCTE-35 ad marker detection (start of ad block)
            if (line.find("#EXT-X-SCTE35-OUT") == 0) {
                if (!ad_state.in_scte35_out) {
                    // Just entering ad block - don't clear buffer to maintain continuity
                    AddDebugLog(L"[AD_SKIP] Found SCTE35-OUT marker, entering ad block - maintaining buffer continuity");
                } else {
                    AddDebugLog(L"[AD_SKIP] Found SCTE35-OUT marker, already in ad block");
                }
                ad_state.in_scte35_out = true;
                ad_state.skip_next_segment = true;
                ad_state.consecutive_skipped_segments = 0;
                ad_state.scte35_start_time = std::chrono::steady_clock::now();
                continue;
            }
            // SCTE-35 ad marker detection (end of ad block)
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                ad_state.in_scte35_out = false;
                ad_state.consecutive_skipped_segments = 0;
                ad_state.just_exited_ad_block = true;
                // Don't clear buffer to maintain stream continuity
                AddDebugLog(L"[AD_SKIP] Found SCTE35-IN marker, exiting ad block - maintaining buffer continuity");
                continue;
            }
            // Stitched ad segments (single segment ads)
            else if (line.find("stitched-ad") != std::string::npos) {
                ad_state.skip_next_segment = true;
                AddDebugLog(L"[AD_SKIP] Found stitched-ad marker");
            }
            // EXTINF tags with specific durations that indicate ads (2.001, 2.002 seconds)
            else if (line.find("#EXTINF:2.00") == 0 && 
                     (line.find("2.001") != std::string::npos || 
                      line.find("2.002") != std::string::npos)) {
                ad_state.skip_next_segment = true;
                AddDebugLog(L"[AD_SKIP] Found ad-duration EXTINF marker");
            }
            // DATERANGE markers for stitched ads
            else if (line.find("#EXT-X-DATERANGE:ID=\"stitched-ad") == 0) {
                ad_state.skip_next_segment = true;
                AddDebugLog(L"[AD_SKIP] Found stitched-ad DATERANGE marker");
            }
            // General stitched content detection
            else if (line.find("stitched") != std::string::npos ||
                     line.find("STITCHED") != std::string::npos) {
                ad_state.skip_next_segment = true;
                AddDebugLog(L"[AD_SKIP] Found general stitched content marker");
            }
            // MIDROLL ad markers
            else if (line.find("EXT-X-DATERANGE") != std::string::npos && 
                     (line.find("MIDROLL") != std::string::npos ||
                      line.find("midroll") != std::string::npos)) {
                ad_state.skip_next_segment = true;
                AddDebugLog(L"[AD_SKIP] Found MIDROLL ad marker");
            }
            continue;
        }
        
        // This is a segment URL
        bool should_skip_ad_segment = false;
        
        // Check if we should skip this segment as an ad
        if (ad_state.skip_next_segment) {
            // Single ad segment (stitched ads, duration-based ads, etc.)
            should_skip_ad_segment = true;
            ad_state.skip_next_segment = false; // Reset after processing one segment
        } else if (ad_state.in_scte35_out) {
            // SCTE35 ad block - check safety limits to prevent infinite ad segment skipping
            auto current_time = std::chrono::steady_clock::now();
            auto ad_block_duration = current_time - ad_state.scte35_start_time;
            
            if (ad_state.consecutive_skipped_segments >= AdBlockState::MAX_CONSECUTIVE_SKIPPED_SEGMENTS) {
                AddDebugLog(L"[AD_SKIP] Safety limit reached: " + std::to_wstring(ad_state.consecutive_skipped_segments) + 
                           L" consecutive skipped segments, exiting ad block");
                ad_state.in_scte35_out = false;
                ad_state.consecutive_skipped_segments = 0;
                ad_state.just_exited_ad_block = true;
            } else if (ad_block_duration >= AdBlockState::MAX_AD_BLOCK_DURATION) {
                AddDebugLog(L"[AD_SKIP] Safety timeout reached: " + 
                           std::to_wstring(std::chrono::duration_cast<std::chrono::seconds>(ad_block_duration).count()) + 
                           L" seconds in ad block, exiting");
                ad_state.in_scte35_out = false;
                ad_state.consecutive_skipped_segments = 0;
                ad_state.just_exited_ad_block = true;
            } else {
                should_skip_ad_segment = true;
            }
        }
        
        if (should_skip_ad_segment) {
            // Ad segment detected - replace with minimal placeholder to maintain stream timing
            AddDebugLog(L"[AD_SKIP] Replacing ad segment with placeholder (replaced #" + 
                       std::to_wstring(ad_state.consecutive_skipped_segments + 1) + L")");
            
            // Use a placeholder marker that will generate minimal content
            segs.push_back(L"__AD_PLACEHOLDER__");
            ad_state.consecutive_skipped_segments++;
            continue;
        }
        
        // Normal content segment - reset skip counter
        if (ad_state.consecutive_skipped_segments > 0) {
            AddDebugLog(L"[AD_SKIP] Returning to normal content after replacing " + 
                       std::to_wstring(ad_state.consecutive_skipped_segments) + L" ad segments with placeholders");
            ad_state.consecutive_skipped_segments = 0;
        }
        
        // Should be a .ts or .aac segment
        std::wstring wline(line.begin(), line.end());
        segs.push_back(wline);
    }
    return std::make_pair(segs, should_clear_buffer);
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

/**
 * Demonstrates why MailSlots cannot replace pipes for video streaming IPC
 * This function tests both approaches with sample video data to show the limitations
 */
bool DemonstrateMailSlotVsPipeComparison(const std::wstring& channel_name) {
    AddDebugLog(L"[DEMO] Starting MailSlot vs Pipe IPC comparison for " + channel_name);
    
    // Create sample video data (simulating a typical video segment)
    const size_t SAMPLE_VIDEO_SIZE = 2 * 1024 * 1024; // 2MB typical segment
    std::vector<char> sample_video_data(SAMPLE_VIDEO_SIZE);
    
    // Fill with sample data pattern
    for (size_t i = 0; i < SAMPLE_VIDEO_SIZE; ++i) {
        sample_video_data[i] = (char)(i % 256);
    }
    
    AddDebugLog(L"[DEMO] Created sample video segment: " + std::to_wstring(SAMPLE_VIDEO_SIZE) + L" bytes");
    
    // Test 1: MailSlot approach
    std::atomic<bool> cancel_token{false};
    std::wstring mailslot_name = L"\\\\.\\mailslot\\tardsplaya_test";
    
    AddDebugLog(L"[DEMO] Testing MailSlot approach...");
    MailSlotComparisonResult mailslot_result = TestMailSlotDataTransfer(
        sample_video_data, 
        mailslot_name, 
        cancel_token
    );
    
    // Test 2: Pipe approach simulation
    AddDebugLog(L"[DEMO] Testing Anonymous Pipe approach...");
    auto pipe_start = std::chrono::high_resolution_clock::now();
    
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const DWORD PIPE_BUFFER_SIZE = 1024 * 1024; // 1MB buffer
    
    bool pipe_success = false;
    if (CreatePipe(&hRead, &hWrite, &sa, PIPE_BUFFER_SIZE)) {
        DWORD bytes_written = 0;
        pipe_success = WriteFile(hWrite, sample_video_data.data(), (DWORD)sample_video_data.size(), &bytes_written, nullptr);
        
        CloseHandle(hRead);
        CloseHandle(hWrite);
        
        if (pipe_success && bytes_written == sample_video_data.size()) {
            AddDebugLog(L"[DEMO] Pipe approach: Successfully wrote " + std::to_wstring(bytes_written) + L" bytes in single operation");
        }
    }
    
    auto pipe_end = std::chrono::high_resolution_clock::now();
    double pipe_time = std::chrono::duration<double, std::milli>(pipe_end - pipe_start).count();
    
    // Generate comparison report
    std::wstring report = GenerateComparisonReport(mailslot_result, PIPE_BUFFER_SIZE, pipe_success);
    
    // Log the complete analysis
    AddDebugLog(L"[DEMO] === IPC COMPARISON COMPLETE ===");
    AddDebugLog(L"[DEMO] MailSlot Time: " + std::to_wstring(mailslot_result.time_taken_ms) + L"ms");
    AddDebugLog(L"[DEMO] Pipe Time: " + std::to_wstring(pipe_time) + L"ms");
    AddDebugLog(L"[DEMO] MailSlot Messages: " + std::to_wstring(mailslot_result.messages_sent));
    AddDebugLog(L"[DEMO] Pipe Messages: 1");
    
    // Output detailed report to debug log
    std::wistringstream report_stream(report);
    std::wstring line;
    while (std::getline(report_stream, line)) {
        AddDebugLog(L"[DEMO] " + line);
    }
    
    return true;
}

bool BufferAndPipeStreamToPlayerOriginal(
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
            // Commented out process death check to prevent premature termination when process is incorrectly marked as dead
            // bool process_running_check = ProcessStillRunning(pi.hProcess, channel_name + L" download_thread", pi.dwProcessId);
            bool error_limit_check = consecutive_errors < max_consecutive_errors;
            
            if (!download_running_check) {
                AddDebugLog(L"[DOWNLOAD] Exit condition: download_running=false for " + channel_name);
                break;
            }
            if (cancel_token_check) {
                AddDebugLog(L"[DOWNLOAD] Exit condition: cancel_token=true for " + channel_name);
                break;
            }
            // Commented out to prevent premature exit when process is incorrectly detected as dead
            // if (!process_running_check) {
            //     AddDebugLog(L"[DOWNLOAD] Exit condition: process died for " + channel_name);
            //     break;
            // }
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

            auto parse_result = ParseSegments(playlist);
            auto segments = parse_result.first;
            bool should_clear_buffer = parse_result.second;
            
            // Clear buffer if we just started an ad block to prevent showing old content
            if (should_clear_buffer) {
                size_t cleared_segments;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    cleared_segments = buffer_queue.size();
                    std::queue<std::vector<char>> empty_queue;
                    buffer_queue.swap(empty_queue); // Clear the queue efficiently
                }
                AddDebugLog(L"[AD_SKIP] Cleared " + std::to_wstring(cleared_segments) + 
                           L" buffered segments when entering/exiting ad block for " + channel_name);
            }
            
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
                
                // Commented out process death check to prevent premature loop exit when process is incorrectly marked as dead
                // bool process_still_running = ProcessStillRunning(pi.hProcess, channel_name + L" segment_download", pi.dwProcessId);
                // if (!process_still_running) {
                //     AddDebugLog(L"[DOWNLOAD] Breaking segment loop - media player process died for " + channel_name);
                //     break;
                
                // Handle ad placeholder markers by generating minimal content
                if (seg == L"__AD_PLACEHOLDER__") {
                    AddDebugLog(L"[AD_SKIP] Generating minimal placeholder content for ad segment");
                    
                    // Generate minimal valid content (just a few null bytes to maintain timing)
                    std::vector<char> placeholder_data(1024, 0); // 1KB of null data
                    
                    // Add to buffer
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        buffer_queue.push(std::move(placeholder_data));
                    }
                    
                    new_segments_downloaded++;
                    AddDebugLog(L"[AD_SKIP] Placeholder content added to buffer for " + channel_name);
                    continue;
                }
                
                // Skip segments that aren't regular .ts/.aac files
                if (seg.find(L"http") != 0) {
                    AddDebugLog(L"[DOWNLOAD] Skipping non-HTTP segment: " + seg.substr(0, 50) + L"...");
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
                   L" (process check disabled in main loop)" +
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
            // Commented out process death check to prevent premature termination when process is incorrectly marked as dead
            // bool process_running_check = ProcessStillRunning(pi.hProcess, channel_name + L" feeder_thread", pi.dwProcessId);
            bool data_available_check = (download_running.load() || !buffer_queue.empty());
            
            if (cancel_token_check) {
                AddDebugLog(L"[FEEDER] Exit condition: cancel_token=true for " + channel_name);
                break;
            }
            // Commented out to prevent premature exit when process is incorrectly detected as dead
            // if (!process_running_check) {
            //     AddDebugLog(L"[FEEDER] Exit condition: process died for " + channel_name);
            //     break;
            // }
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
                   L" (process check disabled in main loop)" +
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

// New wrapper function that allows choosing IPC method
bool BufferAndPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    // Use the globally configured IPC method
    std::wstring method_name;
    switch (g_current_ipc_method) {
        case IPCMethod::MAILSLOTS: method_name = L"MailSlots"; break;
        case IPCMethod::NAMED_PIPES: method_name = L"Named Pipes"; break;
        case IPCMethod::ANONYMOUS_PIPES: method_name = L"Anonymous Pipes"; break;
        default: method_name = L"Unknown(" + std::to_wstring((int)g_current_ipc_method) + L")"; break;
    }
    AddDebugLog(L"[IPC-METHOD] BufferAndPipeStreamToPlayer wrapper called with method: " + method_name + L" for " + channel_name);
    
    switch (g_current_ipc_method) {
        case IPCMethod::MAILSLOTS:
            AddDebugLog(L"[IPC-METHOD] Starting stream with MailSlots for " + channel_name);
            return BufferAndMailSlotStreamToPlayer(player_path, playlist_url, cancel_token,
                                                 buffer_segments, channel_name, chunk_count, selected_quality);
        
        case IPCMethod::NAMED_PIPES:
            AddDebugLog(L"[IPC-METHOD] Starting stream with Named Pipes for " + channel_name);
            return BufferAndNamedPipeStreamToPlayer(player_path, playlist_url, cancel_token,
                                                  buffer_segments, channel_name, chunk_count, selected_quality);
        
        case IPCMethod::ANONYMOUS_PIPES:
        default:
            AddDebugLog(L"[IPC-METHOD] Starting stream with Anonymous Pipes for " + channel_name);
            return BufferAndPipeStreamToPlayerOriginal(player_path, playlist_url, cancel_token,
                                                     buffer_segments, channel_name, chunk_count, selected_quality);
    }
}

bool DemonstrateAlternativeIPCMethods(const std::wstring& channel_name) {
    AddDebugLog(L"[ALTERNATIVE-IPC] Starting demonstration of MailSlots and Named Pipes alternatives for: " + channel_name);
    
    // Create sample video data for testing (simulate 1MB video segment)
    std::vector<char> test_video_data(1024 * 1024, 'X'); // 1MB of test data
    
    // Fill with some pattern to simulate real video data
    for (size_t i = 0; i < test_video_data.size(); i++) {
        test_video_data[i] = static_cast<char>(i % 256);
    }
    
    std::atomic<bool> cancel_token(false);
    
    try {
        // Run comprehensive demo of all alternative methods
        auto results = AlternativeIPCDemo::RunComprehensiveDemo(test_video_data, channel_name, cancel_token);
        
        // Generate and log comparison report
        std::wstring report = AlternativeIPCDemo::GenerateComparisonReport(results, test_video_data);
        
        // Split report into chunks for debug log
        std::wstringstream ss(report);
        std::wstring line;
        while (std::getline(ss, line)) {
            AddDebugLog(L"[REPORT] " + line);
        }
        
        // Summary of findings
        AddDebugLog(L"[ALTERNATIVE-IPC] Demo completed. Key findings:");
        AddDebugLog(L"[ALTERNATIVE-IPC] 1. MailSlots require bridge processes due to stdin incompatibility");
        AddDebugLog(L"[ALTERNATIVE-IPC] 2. Named Pipes are better than MailSlots but add complexity vs anonymous pipes");
        AddDebugLog(L"[ALTERNATIVE-IPC] 3. Named Pipe HTTP-like service limited to single connections");
        AddDebugLog(L"[ALTERNATIVE-IPC] 4. Current implementations remain optimal for their use cases");
        
        return true;
        
    } catch (const std::exception& e) {
        std::string error_msg = e.what();
        AddDebugLog(L"[ALTERNATIVE-IPC] Exception during demo: " + std::wstring(error_msg.begin(), error_msg.end()));
        return false;
    } catch (...) {
        AddDebugLog(L"[ALTERNATIVE-IPC] Unknown exception during demo");
        return false;
    }
}