#pragma once
#include <string>
#include <atomic>

// Memory-mapped file based streaming - replacement for HTTP piping
// Launches the player and streams the HLS data via memory-mapped files instead of HTTP
// Provides a cancel_token for cooperative cancellation. Returns true on success.
// Set cancel_token to true to request stop.
bool BufferAndStreamToPlayerViaMemoryMap(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3, // How many segments to buffer before starting playback
    const std::wstring& channel_name = L"", // Channel name for window title
    std::atomic<int>* chunk_count = nullptr // Pointer to chunk count for status display
);