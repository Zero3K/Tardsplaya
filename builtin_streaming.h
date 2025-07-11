#pragma once

#include "builtin_player.h"
#include <windows.h>
#include <string>
#include <atomic>

// Built-in streaming function that uses the simple internal player
bool BufferAndStreamToBuiltinPlayer(
    HWND hwndStatus,  // Window handle for status display
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
);

// Initialize the built-in player system (simplified - no complex dependencies)
bool InitializeBuiltinPlayerSystem();

// Shutdown the built-in player system
void ShutdownBuiltinPlayerSystem();