#include "stream_thread.h"
#include "stream_pipe.h"
#include "tsduck_transport_router.h"
#include "stream_resource_manager.h"

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
    size_t tab_index,
    const std::wstring& selected_quality,
    StreamingMode mode
) {
    // Check if transport stream mode is requested
    if (mode == StreamingMode::TRANSPORT_STREAM) {
        // Calculate adaptive buffer size for multiple streams to prevent frame drops
        auto& resource_manager = StreamResourceManager::getInstance();
        int active_streams = resource_manager.GetActiveStreamCount();
        
        // Base calculation: segments * 1000 packets per segment
        size_t base_buffer_packets = static_cast<size_t>(buffer_segments * 1000);
        
        // Scale buffer up for multiple streams to maintain quality
        if (active_streams > 1) {
            base_buffer_packets = static_cast<size_t>(base_buffer_packets * 1.5); // 50% larger for multiple streams
        }
        if (active_streams > 3) {
            base_buffer_packets = static_cast<size_t>(base_buffer_packets * 2.0); // Double for many streams
        }
        
        return StartTransportStreamThread(player_path, playlist_url, cancel_token, log_callback,
                                         base_buffer_packets, channel_name, main_window, tab_index);
    }
    
    // Use traditional HLS streaming
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback)
            log_callback(L"Streaming thread started (HLS fallback mode).");
        
        AddDebugLog(L"StartStreamThread: Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index) + 
                   L", BufferSegs=" + std::to_wstring(buffer_segments));
        
        bool ok = BufferAndPipeStreamToPlayer(player_path, playlist_url, cancel_token, buffer_segments, channel_name, chunk_count, selected_quality);
        
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

std::thread StartTransportStreamThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    size_t buffer_packets,
    const std::wstring& channel_name,
    HWND main_window,
    size_t tab_index
) {
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback)
            log_callback(L"TSDuck transport stream thread started.");
        
        AddDebugLog(L"StartTransportStreamThread: Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index) + 
                   L", BufferPackets=" + std::to_wstring(buffer_packets));
        
        try {
            // Create transport stream router
            tsduck_transport::TransportStreamRouter router;
            
            // Configure router
            tsduck_transport::TransportStreamRouter::RouterConfig config;
            config.player_path = player_path;
            config.player_args = L"-";  // Read from stdin
            config.buffer_size_packets = buffer_packets;
            config.enable_adaptation_field = true;
            config.enable_pcr_insertion = true;
            config.pcr_interval = std::chrono::milliseconds(40);
            config.enable_pat_pmt_repetition = true;
            config.pat_pmt_interval = std::chrono::milliseconds(100);
            
            if (log_callback) {
                log_callback(L"[TS_MODE] Starting TSDuck transport stream routing");
                log_callback(L"[TS_MODE] Buffer: " + std::to_wstring(buffer_packets) + L" packets (~" + 
                            std::to_wstring((buffer_packets * 188) / 1024) + L"KB)");
            }
            
            // Start routing
            bool routing_started = router.StartRouting(playlist_url, config, cancel_token, log_callback);
            
            if (routing_started) {
                if (log_callback) {
                    log_callback(L"[TS_MODE] Transport stream routing active");
                }
                
                // Monitor routing while active
                while (router.IsRouting() && !cancel_token) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    // Optionally report buffer status
                    auto stats = router.GetBufferStats();
                    if (log_callback && stats.total_packets_processed % 1000 == 0) {
                        log_callback(L"[TS_MODE] Buffer: " + std::to_wstring(stats.buffered_packets) + 
                                   L" packets, Utilization: " + std::to_wstring(int(stats.buffer_utilization * 100)) + L"%");
                    }
                }
                
                if (log_callback) {
                    log_callback(L"[TS_MODE] Transport stream routing completed");
                }
            } else {
                if (log_callback) {
                    log_callback(L"[TS_MODE] Failed to start transport stream routing");
                }
            }
            
        } catch (const std::exception& e) {
            std::string error_msg = e.what();
            if (log_callback) {
                log_callback(L"[TS_MODE] Transport stream error: " + 
                           std::wstring(error_msg.begin(), error_msg.end()));
            }
            AddDebugLog(L"StartTransportStreamThread: Exception: " + 
                       std::wstring(error_msg.begin(), error_msg.end()));
        }
        
        AddDebugLog(L"StartTransportStreamThread: Stream finished, Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index));
        
        if (log_callback) {
            bool user_stopped = cancel_token.load();
            
            if (user_stopped) {
                log_callback(L"[TS_MODE] Transport stream stopped by user.");
            } else {
                log_callback(L"[TS_MODE] Transport stream ended normally.");
                // Post auto-stop message for this specific tab
                if (main_window && tab_index != SIZE_MAX) {
                    AddDebugLog(L"StartTransportStreamThread: Posting auto-stop for tab " + std::to_wstring(tab_index));
                    PostMessage(main_window, WM_USER + 2, (WPARAM)tab_index, 0);
                }
            }
        }
    });
}