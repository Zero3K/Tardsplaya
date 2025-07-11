#pragma once
#include <string>
#include <atomic>

// Launches the player and pipes the HLS stream (playlist_url) to its stdin via memory-backed files.
// Provides a cancel_token for cooperative cancellation. Returns true on success.
// Set cancel_token to true to request stop.
bool BufferAndPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3, // How many segments to buffer before starting playback
    const std::wstring& channel_name = L"", // Channel name for window title
    std::atomic<int>* chunk_count = nullptr // Pointer to chunk count for status display
);

// Legacy HTTP-based streaming (kept for backwards compatibility if needed)
bool BufferAndPipeStreamToPlayerHTTP(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3, // How many segments to buffer before starting playback
    const std::wstring& channel_name = L"", // Channel name for window title
    std::atomic<int>* chunk_count = nullptr // Pointer to chunk count for status display
);