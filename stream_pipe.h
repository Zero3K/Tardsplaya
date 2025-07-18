#pragma once
#include <string>
#include <atomic>
#include <memory>

// Forward declaration for semaphore class
class ProducerConsumerSemaphores;

// Launches the player and pipes the HLS stream (playlist_url) to its stdin with a buffer.
// Provides a cancel_token for cooperative cancellation. Returns true on success.
// Set cancel_token to true to request stop.
// Now includes semaphore-based IPC for better flow control
bool BufferAndPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3, // How many segments to buffer before starting playback
    const std::wstring& channel_name = L"", // Channel name for window title
    std::atomic<int>* chunk_count = nullptr, // Pointer to chunk count for status display
    const std::wstring& selected_quality = L"", // User's selected quality for ad recovery
    bool use_semaphore_ipc = true // Enable semaphore-based IPC for flow control
);