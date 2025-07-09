#pragma once
#include <string>
#include <atomic>

// Launches the player and streams the HLS content via local TCP socket connection.
// CHANGED: Replaced named pipes with TCP sockets for better multi-stream reliability.
// TCP sockets provide better flow control and error handling than pipes in multi-stream scenarios.
// Each stream gets its own TCP port and the media player connects to http://localhost:port/
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