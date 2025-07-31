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

// Forward declaration for HTTP server
namespace tardsplaya {
    class HttpStreamServer;
}

// Browser version: Downloads HLS stream and serves to HTTP server for browser playback
bool BufferAndServeStreamToBrowser(
    tardsplaya::HttpStreamServer* http_server,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3, // How many segments to buffer before starting serving
    const std::wstring& channel_name = L"", // Channel name for logging
    std::atomic<int>* chunk_count = nullptr, // Pointer to chunk count for status display
    const std::wstring& selected_quality = L"" // User's selected quality for ad recovery
);