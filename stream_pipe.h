#pragma once
#include <string>
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Launches the player and pipes the HLS stream (playlist_url) to its stdin with a buffer.
// Provides a cancel_token for cooperative cancellation. Returns true on success.
// Set cancel_token to true to request stop.
bool BufferAndPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3, // How many segments to buffer before starting playback
    const std::wstring& channel_name = L"", // Channel name for window title
    std::atomic<int>* chunk_count = nullptr, // Pointer to chunk count for status display
    const std::wstring& selected_quality = L"", // User's selected quality for ad recovery
    HANDLE* player_process_handle = nullptr // Optional pointer to store player process handle
);

// NOTE: BufferAndPipeStreamToPlayerDatapath is now declared in datapath_ipc.h

// Legacy Windows pipes implementation (preserved for compatibility)
bool BufferAndPipeStreamToPlayerLegacy(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3,
    const std::wstring& channel_name = L"",
    std::atomic<int>* chunk_count = nullptr,
    const std::wstring& selected_quality = L"",
    HANDLE* player_process_handle = nullptr
);