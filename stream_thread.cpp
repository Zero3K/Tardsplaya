#include "stream_thread.h"
#include "stream_pipe.h"
#include <windows.h>

std::thread StartStreamThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    std::atomic<bool>* user_requested_stop,
    HWND main_window,
    size_t tab_index
) {
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback)
            log_callback(L"Streaming thread started.");
        
        AddDebugLog(L"StartStreamThread: Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index) + 
                   L", BufferSegs=" + std::to_wstring(buffer_segments));
        
        bool ok = BufferAndPipeStreamToPlayer(player_path, playlist_url, cancel_token, buffer_segments, channel_name, chunk_count);
        
        AddDebugLog(L"StartStreamThread: Stream finished, ok=" + std::to_wstring(ok) + 
                   L", Channel=" + channel_name + L", Tab=" + std::to_wstring(tab_index));
        
        if (log_callback) {
            // Check if user explicitly requested stop
            bool user_stopped = user_requested_stop && user_requested_stop->load();
            AddDebugLog(L"StartStreamThread: user_stopped=" + std::to_wstring(user_stopped) + 
                       L", Channel=" + channel_name);
            
            if (user_stopped) {
                log_callback(L"Streaming stopped by user.");
            } else if (ok) {
                log_callback(L"Stream ended normally.");
                // Post auto-stop message for this specific tab
                if (main_window && tab_index != SIZE_MAX) {
                    AddDebugLog(L"StartStreamThread: Posting auto-stop for tab " + std::to_wstring(tab_index));
                    PostMessage(main_window, WM_USER + 2, (WPARAM)tab_index, 0);
                }
            } else {
                log_callback(L"Streaming failed or was interrupted.");
            }
        }
    });
}