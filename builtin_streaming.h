#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX      // Prevent Windows min/max macros
#endif
#include <windows.h>
#include "builtin_player.h"
#include <string>
#include <atomic>

// Built-in streaming function that uses the simple internal player
bool BufferAndStreamToBuiltinPlayer(
    HWND hwndStatus,  // Window handle for status display
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    const std::wstring& quality,  // Quality string for window sizing
    std::atomic<int>* chunk_count
);

// Get the global builtin player instance (thread-safe)
SimpleBuiltinPlayer* GetBuiltinPlayer();

// Get the global builtin player mutex
std::mutex& GetBuiltinPlayerMutex();

// Initialize the built-in player system (simplified - no complex dependencies)
bool InitializeBuiltinPlayerSystem();

// Shutdown the built-in player system
void ShutdownBuiltinPlayerSystem();