#include "stream_thread.h"
#include "stream_pipe.h"

std::thread StartStreamThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
) {
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback)
            log_callback(L"Streaming thread started.");
        bool ok = BufferAndPipeStreamToPlayer(player_path, playlist_url, cancel_token, buffer_segments, channel_name, chunk_count);
        if (log_callback) {
            if (cancel_token.load())
                log_callback(L"Streaming stopped by user.");
            else if (ok)
                log_callback(L"Stream ended normally.");
            else
                log_callback(L"Streaming failed or was interrupted.");
        }
    });
}