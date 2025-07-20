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
    StreamingMode mode,
    HANDLE* player_process_handle,
    bool enable_ad_skipping
) {
    // Check if transport stream mode is requested
    if (mode == StreamingMode::TRANSPORT_STREAM) {
        // For low-latency streaming, use smaller buffers to reduce delay
        size_t base_buffer_packets = 3000;  // Reduced from 1000*segments to fixed 3000 for low latency
        
        // For multiple streams, still scale but keep it reasonable for latency
        auto& resource_manager = StreamResourceManager::getInstance();
        int active_streams = resource_manager.GetActiveStreamCount();
        
        if (active_streams > 1) {
            base_buffer_packets = static_cast<size_t>(base_buffer_packets * 1.2); // Only 20% larger
        }
        if (active_streams > 3) {
            base_buffer_packets = static_cast<size_t>(base_buffer_packets * 1.5); // Max 50% larger
        }
        
        return StartTransportStreamThread(player_path, playlist_url, cancel_token, log_callback,
                                         base_buffer_packets, channel_name, chunk_count, main_window, tab_index, player_process_handle, enable_ad_skipping);
    }
    
    // Use traditional HLS streaming
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback)
            log_callback(L"Streaming thread started (HLS fallback mode).");
        
        AddDebugLog(L"StartStreamThread: Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index) + 
                   L", BufferSegs=" + std::to_wstring(buffer_segments));
        
        bool ok = BufferAndPipeStreamToPlayer(player_path, playlist_url, cancel_token, buffer_segments, channel_name, chunk_count, selected_quality, player_process_handle);
        
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
    std::atomic<int>* chunk_count,
    HWND main_window,
    size_t tab_index,
    HANDLE* player_process_handle,
    bool enable_ad_skipping
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
            
            // Configure router with low-latency optimizations
            tsduck_transport::TransportStreamRouter::RouterConfig config;
            config.player_path = player_path;
            config.player_args = L"-";  // Read from stdin
            config.buffer_size_packets = buffer_packets;
            config.enable_adaptation_field = true;
            config.enable_pcr_insertion = true;
            config.pcr_interval = std::chrono::milliseconds(40);
            config.enable_pat_pmt_repetition = true;
            config.pat_pmt_interval = std::chrono::milliseconds(100);
            
            // Enable low-latency mode for reduced stream delay
            config.low_latency_mode = true;
            config.max_segments_to_buffer = 2;  // Only buffer latest 2 segments
            config.playlist_refresh_interval = std::chrono::milliseconds(500);  // Check every 500ms
            config.skip_old_segments = true;
            
            // Configure ad skipping based on user preference
            config.enable_ad_skipping = enable_ad_skipping;
            config.skip_scte35_ads = enable_ad_skipping;  // Use SCTE-35 markers when ad skipping is enabled
            config.skip_pattern_detected_ads = enable_ad_skipping;  // Use pattern detection when enabled
            config.maintain_stream_continuity = true;  // Always maintain smooth playback
            config.max_ad_break_duration = std::chrono::milliseconds(300000);  // Max 5-minute ad breaks
            config.log_ad_skipping = true;  // Enable ad skipping logging
            
            if (log_callback) {
                log_callback(L"[TS_MODE] Starting TSDuck transport stream routing");
                log_callback(L"[TS_MODE] Buffer: " + std::to_wstring(buffer_packets) + L" packets (~" + 
                            std::to_wstring((buffer_packets * 188) / 1024) + L"KB)");
                if (enable_ad_skipping) {
                    log_callback(L"[AD_SKIP] Ad skipping enabled (SCTE-35 + pattern detection)");
                } else {
                    log_callback(L"[AD_SKIP] Ad skipping disabled - all segments will be played");
                }
            }
            
            // Start routing
            bool routing_started = router.StartRouting(playlist_url, config, cancel_token, log_callback);
            
            if (routing_started) {
                // Store player process handle if pointer provided
                if (player_process_handle) {
                    *player_process_handle = router.GetPlayerProcessHandle();
                }
                
                if (log_callback) {
                    log_callback(L"[TS_MODE] Transport stream routing active");
                }
                
                // Monitor routing while active
                while (router.IsRouting() && !cancel_token) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    // Report buffer and frame statistics
                    auto stats = router.GetBufferStats();
                    
                    // Update chunk count for status bar (convert frames to approximate chunks)
                    if (chunk_count) {
                        *chunk_count = static_cast<int>(stats.buffered_packets);
                    }
                    
                    // Periodic logging with frame information
                    if (log_callback && stats.total_packets_processed % 1000 == 0) {
                        std::wstring status_msg = L"[TS_MODE] Buffer: " + std::to_wstring(stats.buffered_packets) + 
                                   L" packets, Utilization: " + std::to_wstring(int(stats.buffer_utilization * 100)) + L"%";
                        
                        // Add frame information if available
                        if (stats.total_frames_processed > 0) {
                            status_msg += L", Frames: " + std::to_wstring(stats.total_frames_processed);
                            if (stats.current_fps > 0) {
                                status_msg += L", FPS: " + std::to_wstring(static_cast<int>(stats.current_fps));
                            }
                            if (stats.frames_dropped > 0) {
                                status_msg += L", Dropped: " + std::to_wstring(stats.frames_dropped);
                            }
                        }
                        
                        // Add video/audio health information
                        if (stats.video_packets_processed > 0 || stats.audio_packets_processed > 0) {
                            status_msg += L", Video: " + std::to_wstring(stats.video_packets_processed) + 
                                         L", Audio: " + std::to_wstring(stats.audio_packets_processed);
                            
                            if (!stats.video_stream_healthy) {
                                status_msg += L" [VIDEO_UNHEALTHY]";
                            }
                            if (!stats.audio_stream_healthy) {
                                status_msg += L" [AUDIO_UNHEALTHY]";
                            }
                            if (stats.video_sync_loss_count > 0) {
                                status_msg += L" [SYNC_LOSS:" + std::to_wstring(stats.video_sync_loss_count) + L"]";
                            }
                        }
                        
                        log_callback(status_msg);
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