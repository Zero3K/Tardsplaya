#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <vector>

/**
 * Alternative IPC implementations to replace the current anonymous pipe approach
 * These provide actual working implementations (not just demonstrations) of:
 * 1. MailSlot-based streaming with bridge process
 * 2. Named Pipe-based streaming
 */

enum class IPCMethod {
    ANONYMOUS_PIPES,  // Current default method
    MAILSLOTS,        // MailSlots with bridge process
    NAMED_PIPES       // Named pipes
};

/**
 * MailSlot-based streaming implementation
 * Uses a bridge process to convert MailSlot messages to stdin stream
 */
bool BufferAndMailSlotStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
);

/**
 * Named Pipe-based streaming implementation
 * Uses named pipes instead of anonymous pipes
 */
bool BufferAndNamedPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
);

/**
 * Create a bridge process for MailSlot to stdin conversion
 */
HANDLE CreateMailSlotBridge(const std::wstring& mailslot_name, HANDLE& stdin_write_handle);

/**
 * Send video segment data via MailSlot
 */
bool SendVideoSegmentViaMailSlot(HANDLE mailslot_client, const std::vector<char>& segment_data);

/**
 * Create and configure named pipe for streaming
 */
HANDLE CreateStreamingNamedPipe(const std::wstring& pipe_name, HANDLE& client_handle);

/**
 * Wrapper function that uses the selected IPC method
 */
bool StreamToPlayerWithIPC(
    IPCMethod method,
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
);

// Global configuration for IPC method selection
extern IPCMethod g_current_ipc_method;