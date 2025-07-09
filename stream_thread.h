#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <functional>

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
    size_t tab_index = 0
);