#include "stream_thread.h"
#include "stream_pipe.h"
#include "tsduck_transport_router.h"
#include "stream_resource_manager.h"
#include "tx_queue_ipc.h"
#include "http_server.h"
#include <shellapi.h>

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
    HANDLE* player_process_handle
) {
    // Check for Browser Playback mode (new mpegts.js browser streaming)
    if (mode == StreamingMode::BROWSER_PLAYBACK) {
        return std::thread([=, &cancel_token]() mutable {
            if (log_callback)
                log_callback(L"Starting Browser Playback streaming thread for " + channel_name);
            
            AddDebugLog(L"StartStreamThread: Browser Playback mode - Channel=" + channel_name + 
                       L", Tab=" + std::to_wstring(tab_index) + 
                       L", BufferSegs=" + std::to_wstring(buffer_segments));
            
            try {
                // Create HTTP server for serving stream data
                auto http_server = std::make_unique<tardsplaya::HttpStreamServer>();
                
                // Start HTTP server on a random available port (8080 + tab_index to avoid conflicts)
                int port = 8080 + static_cast<int>(tab_index);
                if (!http_server->StartServer(port)) {
                    // Try alternative ports if the preferred one is in use
                    for (int retry_port = 8090; retry_port < 8100; ++retry_port) {
                        if (http_server->StartServer(retry_port)) {
                            port = retry_port;
                            break;
                        }
                    }
                    
                    if (!http_server->IsRunning()) {
                        if (log_callback) {
                            log_callback(L"[BROWSER] Failed to start HTTP server on any port");
                        }
                        return;
                    }
                }
                
                std::wstring stream_url = http_server->GetStreamUrl();
                if (log_callback) {
                    log_callback(L"[BROWSER] HTTP server started on port " + std::to_wstring(port));
                    log_callback(L"[BROWSER] Player URL: " + stream_url);
                }
                
                // Launch browser to view the stream
                ShellExecuteW(nullptr, L"open", stream_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                
                // Simulate player process for compatibility (browser window)
                if (player_process_handle) {
                    // We can't easily track browser process, so use a dummy handle
                    *player_process_handle = GetCurrentProcess(); // Placeholder
                }
                
                // Start downloading and serving stream segments
                if (log_callback) {
                    log_callback(L"[BROWSER] Starting stream download and HTTP serving");
                }
                
                // Use browser buffering and serving function
                bool stream_success = BufferAndServeStreamToBrowser(
                    http_server.get(), playlist_url, cancel_token, buffer_segments, 
                    channel_name, chunk_count, selected_quality
                );
                
                // Stop HTTP server
                http_server->StopServer();
                
                if (log_callback) {
                    log_callback(L"[BROWSER] Browser streaming completed for " + channel_name);
                }
                
                // Handle stream end
                if (log_callback) {
                    bool user_stopped = user_requested_stop && user_requested_stop->load();
                    
                    if (user_stopped) {
                        log_callback(L"[BROWSER] Browser streaming stopped by user.");
                    } else if (stream_success) {
                        log_callback(L"[BROWSER] Browser stream ended normally.");
                        // Post auto-stop message for this specific tab
                        if (main_window && tab_index != SIZE_MAX) {
                            AddDebugLog(L"StartStreamThread: Posting auto-stop for browser tab " + std::to_wstring(tab_index));
                            PostMessage(main_window, WM_USER + 2, (WPARAM)tab_index, 0);
                        }
                    } else {
                        log_callback(L"[BROWSER] Browser streaming failed or was interrupted.");
                    }
                }
                
            } catch (const std::exception& e) {
                std::string error_msg = e.what();
                if (log_callback) {
                    log_callback(L"[BROWSER] Error: " + std::wstring(error_msg.begin(), error_msg.end()));
                }
                AddDebugLog(L"StartStreamThread: Browser mode exception: " + 
                           std::wstring(error_msg.begin(), error_msg.end()));
            }
            
            AddDebugLog(L"StartStreamThread: Browser stream finished, Channel=" + channel_name + 
                       L", Tab=" + std::to_wstring(tab_index));
        });
    }
    
    // Check for TX-Queue IPC mode (new high-performance mode)
    if (mode == StreamingMode::TX_QUEUE_IPC) {
        return std::thread([=, &cancel_token]() mutable {
            if (log_callback)
                log_callback(L"Starting TX-Queue IPC streaming thread for " + channel_name);
            
            AddDebugLog(L"StartStreamThread: TX-Queue IPC mode - Channel=" + channel_name + 
                       L", Tab=" + std::to_wstring(tab_index) + 
                       L", BufferSegs=" + std::to_wstring(buffer_segments));
            
            try {
                // Create TX-Queue stream manager
                auto stream_manager = std::make_unique<tardsplaya::TxQueueStreamManager>(player_path, channel_name);
                
                // Initialize the streaming system
                if (!stream_manager->Initialize()) {
                    if (log_callback) {
                        log_callback(L"[TX-QUEUE] Failed to initialize streaming system");
                    }
                    return;
                }
                
                // Store player process handle if pointer provided
                if (player_process_handle) {
                    *player_process_handle = stream_manager->GetPlayerProcess();
                }
                
                // Start streaming
                if (!stream_manager->StartStreaming(playlist_url, cancel_token, log_callback, chunk_count)) {
                    if (log_callback) {
                        log_callback(L"[TX-QUEUE] Failed to start streaming");
                    }
                    return;
                }
                
                if (log_callback) {
                    log_callback(L"[TX-QUEUE] TX-Queue IPC streaming active for " + channel_name);
                }
                
                // Monitor streaming while active
                while (stream_manager->IsStreaming() && !cancel_token.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    
                    // Report statistics
                    auto stats = stream_manager->GetStats();
                    
                    // Update chunk count for status bar
                    if (chunk_count) {
                        *chunk_count = static_cast<int>(stats.segments_produced - stats.segments_consumed);
                    }
                    
                    // Periodic logging with detailed statistics
                    if (log_callback && stats.segments_produced % 10 == 0 && stats.segments_produced > 0) {
                        std::wstring status_msg = L"[TX-QUEUE] Segments: " + std::to_wstring(stats.segments_produced) + 
                                   L" produced, " + std::to_wstring(stats.segments_consumed) + L" consumed";
                        
                        if (stats.segments_dropped > 0) {
                            status_msg += L", " + std::to_wstring(stats.segments_dropped) + L" dropped";
                        }
                        
                        status_msg += L", " + std::to_wstring(stats.bytes_transferred / 1024) + L"KB transferred";
                        
                        if (!stats.player_running) {
                            status_msg += L" [PLAYER_DEAD]";
                        }
                        if (!stats.queue_ready) {
                            status_msg += L" [QUEUE_ERROR]";
                        }
                        
                        log_callback(status_msg);
                    }
                    
                    // Check if player died
                    if (!stats.player_running) {
                        if (log_callback) {
                            log_callback(L"[TX-QUEUE] Player process died, stopping streaming");
                        }
                        break;
                    }
                }
                
                // Stop streaming
                stream_manager->StopStreaming();
                
                if (log_callback) {
                    log_callback(L"[TX-QUEUE] TX-Queue IPC streaming completed for " + channel_name);
                }
                
            } catch (const std::exception& e) {
                std::string error_msg = e.what();
                if (log_callback) {
                    log_callback(L"[TX-QUEUE] Error: " + std::wstring(error_msg.begin(), error_msg.end()));
                }
                AddDebugLog(L"StartStreamThread: TX-Queue exception: " + 
                           std::wstring(error_msg.begin(), error_msg.end()));
            }
            
            AddDebugLog(L"StartStreamThread: TX-Queue stream finished, Channel=" + channel_name + 
                       L", Tab=" + std::to_wstring(tab_index));
            
            if (log_callback) {
                bool user_stopped = user_requested_stop && user_requested_stop->load();
                
                if (user_stopped) {
                    log_callback(L"[TX-QUEUE] Streaming stopped by user.");
                } else {
                    log_callback(L"[TX-QUEUE] Stream ended normally.");
                    // Post auto-stop message for this specific tab
                    if (main_window && tab_index != SIZE_MAX) {
                        AddDebugLog(L"StartStreamThread: Posting auto-stop for tab " + std::to_wstring(tab_index));
                        PostMessage(main_window, WM_USER + 2, (WPARAM)tab_index, 0);
                    }
                }
            }
        });
    }
    
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
                                         base_buffer_packets, channel_name, chunk_count, main_window, tab_index, player_process_handle);
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
    HANDLE* player_process_handle
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
            
            if (log_callback) {
                log_callback(L"[TS_MODE] Starting TSDuck transport stream routing");
                log_callback(L"[TS_MODE] Buffer: " + std::to_wstring(buffer_packets) + L" packets (~" + 
                            std::to_wstring((buffer_packets * 188) / 1024) + L"KB)");
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