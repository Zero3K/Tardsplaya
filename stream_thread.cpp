#include "stream_thread.h"
#include "stream_pipe.h"

std::thread StartStreamThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    int buffer_segments,
    const std::wstring& channel_name
) {
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback)
            log_callback(L"Streaming thread started.");
        bool ok = BufferAndPipeStreamToPlayer(player_path, playlist_url, cancel_token, buffer_segments, channel_name);
        if (log_callback) {
            if (ok)
                log_callback(L"Player exited normally.");
            else if (cancel_token)
                log_callback(L"Streaming stopped by user.");
            else
                log_callback(L"Streaming failed or was interrupted.");
        }
    });
}