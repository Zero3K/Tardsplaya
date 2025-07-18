#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX  // Prevent min/max macro conflicts
#include <windows.h>

// Forward declarations for debug logging
extern bool g_verboseDebug;
void AddDebugLog(const std::wstring& msg);

// Streaming mode enumeration
enum class StreamingMode {
    HLS_SEGMENTS,      // Traditional HLS segment-based streaming (default)
    TRANSPORT_STREAM   // TSDuck-inspired transport stream routing
};

// Launches a thread to buffer and pipe the stream.
// The callback is called with a log/status message (can be nullptr).
// Returns the std::thread object (detached or to be joined by caller).
std::thread StartStreamThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback = nullptr,
    int buffer_segments = 3,
    const std::wstring& channel_name = L"",
    std::atomic<int>* chunk_count = nullptr,
    std::atomic<bool>* user_requested_stop = nullptr,
    HWND main_window = nullptr,
    size_t tab_index = 0,
    const std::wstring& selected_quality = L"",
    StreamingMode mode = StreamingMode::HLS_SEGMENTS
);

// Start TSDuck transport stream routing (alternative to traditional HLS streaming)
std::thread StartTransportStreamThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback = nullptr,
    size_t buffer_packets = 5000,
    const std::wstring& channel_name = L"",
    HWND main_window = nullptr,
    size_t tab_index = 0
);