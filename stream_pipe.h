#pragma once
#include <string>
#include <atomic>

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
    const std::wstring& selected_quality = L"" // User's selected quality for ad recovery
);

/**
 * Demonstrates why MailSlots cannot replace pipes for video streaming IPC
 * This function tests both approaches with sample video data to show the limitations
 */
bool DemonstrateMailSlotVsPipeComparison(const std::wstring& channel_name);