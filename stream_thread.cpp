#include "stream_thread.h"
#include "stream_pipe.h"
#include "tsduck_transport_router.h"
#include "stream_resource_manager.h"
#include "tardsplaya_hls_reclock_integration.h"
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// Helper function to check if a path is a local file vs URL
static bool IsLocalFile(const std::wstring& path) {
    // Check if it's a local file path (not HTTP/HTTPS URL)
    if (path.find(L"http://") == 0 || path.find(L"https://") == 0) {
        return false;
    }
    
    // Check if it has file extension typical of processed files
    if (path.find(L".ts") != std::wstring::npos || path.find(L".flv") != std::wstring::npos) {
        return true;
    }
    
    return false;
}

// Helper function to stream local file directly to player
static std::thread StartLocalFileStreamThread(
    const std::wstring& player_path,
    const std::wstring& file_path,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name,
    HWND main_window,
    size_t tab_index,
    HANDLE* player_process_handle
) {
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback) {
            log_callback(L"Starting direct file streaming for corrected HLS stream");
        }
        
        AddDebugLog(L"StartLocalFileStreamThread: File=" + file_path + L", Channel=" + channel_name);
        
        try {
            // Launch the player process with stdin input
            STARTUPINFOW si = {};
            PROCESS_INFORMATION pi = {};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = NULL; // We'll create a pipe
            
            // Create pipe for streaming
            HANDLE hReadPipe, hWritePipe;
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            
            if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
                if (log_callback) {
                    log_callback(L"Failed to create pipe for player");
                }
                return;
            }
            
            si.hStdInput = hReadPipe;
            
            // Build command line
            std::wstring cmdline = L"\"" + player_path + L"\" -";
            
            BOOL success = CreateProcessW(
                NULL,
                const_cast<wchar_t*>(cmdline.c_str()),
                NULL, NULL, TRUE,
                0, NULL, NULL,
                &si, &pi
            );
            
            CloseHandle(hReadPipe); // Close the read end in parent process
            
            if (!success) {
                CloseHandle(hWritePipe);
                if (log_callback) {
                    log_callback(L"Failed to launch player for corrected stream");
                }
                return;
            }
            
            if (player_process_handle) {
                *player_process_handle = pi.hProcess;
            }
            
            if (log_callback) {
                log_callback(L"Player launched, streaming corrected file data...");
            }
            
            // Open the corrected file and stream it to the player
            std::string narrow_path(file_path.begin(), file_path.end());
            std::ifstream file(narrow_path, std::ios::binary);
            
            if (!file.is_open()) {
                CloseHandle(hWritePipe);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                if (log_callback) {
                    log_callback(L"Failed to open corrected stream file");
                }
                return;
            }
            
            // Stream file data to player
            const size_t buffer_size = 188 * 1000; // Stream in 1000 TS packet chunks
            std::vector<char> buffer(buffer_size);
            DWORD bytes_written;
            
            while (!cancel_token.load() && file.good()) {
                file.read(buffer.data(), buffer_size);
                std::streamsize bytes_read = file.gcount();
                
                if (bytes_read > 0) {
                    if (!WriteFile(hWritePipe, buffer.data(), static_cast<DWORD>(bytes_read), &bytes_written, NULL)) {
                        break; // Player closed or error
                    }
                }
            }
            
            file.close();
            CloseHandle(hWritePipe);
            
            if (log_callback) {
                log_callback(L"Corrected stream playback completed");
            }
            
            // Wait a bit for player to finish processing
            WaitForSingleObject(pi.hProcess, 2000);
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            
        } catch (const std::exception& e) {
            if (log_callback) {
                std::wstring error_msg(e.what(), e.what() + strlen(e.what()));
                log_callback(L"Error in local file streaming: " + error_msg);
            }
        }
        
        AddDebugLog(L"StartLocalFileStreamThread: Completed for " + channel_name);
    });
}

// Helper function to stream HLS with PTS correction directly to player via pipe
static std::thread StartHLSReclockPipeThread(
    const std::wstring& player_path,
    const std::wstring& hls_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name,
    HWND main_window,
    size_t tab_index,
    HANDLE* player_process_handle
) {
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback) {
            log_callback(L"Starting HLS stream with direct PTS correction piping");
        }
        
        AddDebugLog(L"StartHLSReclockPipeThread: URL=" + hls_url + L", Channel=" + channel_name);
        
        try {
            using namespace tardsplaya_hls_reclock;
            
            // Convert wide string to narrow string for the reclock API
            std::string narrow_url(hls_url.begin(), hls_url.end());
            
            // Create configuration for live streams
            auto config = integration_utils::CreateConfigForStreamType("live");
            config.verbose_logging = g_verboseDebug;
            
            // Create reclock processor
            TardsplayaHLSReclock reclocker(config);
            
            if (!reclocker.IsReclockToolAvailable()) {
                if (log_callback) {
                    log_callback(L"HLS PTS reclock tool not available for pipe streaming");
                }
                return;
            }
            
            // Create pipe for streaming
            HANDLE hReadPipe, hWritePipe;
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            
            if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
                if (log_callback) {
                    log_callback(L"Failed to create pipe for HLS PTS reclock streaming");
                }
                return;
            }
            
            // Launch the player process with stdin input
            STARTUPINFOW si = {};
            PROCESS_INFORMATION pi = {};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = hReadPipe;
            
            // Build command line
            std::wstring cmdline = L"\"" + player_path + L"\" -";
            
            BOOL success = CreateProcessW(
                NULL,
                const_cast<wchar_t*>(cmdline.c_str()),
                NULL, NULL, TRUE,
                0, NULL, NULL,
                &si, &pi
            );
            
            CloseHandle(hReadPipe); // Close the read end in parent process
            
            if (!success) {
                CloseHandle(hWritePipe);
                if (log_callback) {
                    log_callback(L"Failed to launch player for HLS PTS reclock streaming");
                }
                return;
            }
            
            if (player_process_handle) {
                *player_process_handle = pi.hProcess;
            }
            
            if (log_callback) {
                log_callback(L"Player launched, starting HLS PTS reclock streaming...");
            }
            
            // Process progress callback
            auto progress_cb = [&](int progress, const std::string& status) {
                if (log_callback && g_verboseDebug) {
                    std::wstring wide_status(status.begin(), status.end());
                    log_callback(L"PTS Reclock Pipe: " + wide_status + L" (" + std::to_wstring(progress) + L"%)");
                }
            };
            
            // Start the HLS PTS reclock tool with pipe output
            bool reclock_success = reclocker.ProcessHLSStreamToPipe(narrow_url, hWritePipe, "mpegts", progress_cb);
            
            if (!reclock_success) {
                if (log_callback) {
                    log_callback(L"Failed to start HLS PTS reclock pipe streaming");
                }
                CloseHandle(hWritePipe);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return;
            }
            
            if (log_callback) {
                log_callback(L"HLS PTS reclock pipe streaming active");
            }
            
            // Wait for either cancellation or player to finish
            while (!cancel_token.load()) {
                DWORD wait_result = WaitForSingleObject(pi.hProcess, 1000);
                if (wait_result == WAIT_OBJECT_0) {
                    // Player process finished
                    break;
                }
            }
            
            // Cleanup
            CloseHandle(hWritePipe);
            
            if (log_callback) {
                log_callback(L"HLS PTS reclock pipe streaming completed");
            }
            
            // Wait a bit for player to finish processing
            WaitForSingleObject(pi.hProcess, 2000);
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            
        } catch (const std::exception& e) {
            if (log_callback) {
                std::wstring error_msg(e.what(), e.what() + strlen(e.what()));
                log_callback(L"Error in HLS PTS reclock pipe streaming: " + error_msg);
            }
        }
        
        AddDebugLog(L"StartHLSReclockPipeThread: Completed for " + channel_name);
    });
}

// Helper function to process HLS streams with PTS discontinuity correction
static std::wstring ProcessHLSWithPTSCorrection(
    const std::wstring& playlist_url, 
    const std::wstring& channel_name,
    std::function<void(const std::wstring&)> log_callback
) {
    using namespace tardsplaya_hls_reclock;
    
    // Convert wide string to narrow string for the reclock API
    std::string narrow_url(playlist_url.begin(), playlist_url.end());
    
    // Check if we should apply PTS correction
    if (!integration_utils::ShouldApplyPTSCorrection(narrow_url)) {
        return playlist_url; // Return original URL
    }
    
    if (log_callback) {
        log_callback(L"Applying HLS PTS discontinuity correction for " + channel_name);
    }
    
    // Create configuration for live streams (most Twitch streams benefit from this)
    auto config = integration_utils::CreateConfigForStreamType("live");
    config.verbose_logging = g_verboseDebug;
    
    // Create reclock processor
    TardsplayaHLSReclock reclocker(config);
    
    if (!reclocker.IsReclockToolAvailable()) {
        if (log_callback) {
            log_callback(L"HLS PTS reclock tool not available, using original stream");
        }
        AddDebugLog(L"HLS PTS reclock tool not found, falling back to original stream");
        return playlist_url; // Return original URL if tool not available
    }
    
    // Process the stream with progress callback
    auto progress_cb = [&](int progress, const std::string& status) {
        if (log_callback && g_verboseDebug) {
            std::wstring wide_status(status.begin(), status.end());
            log_callback(L"PTS Reclock: " + wide_status + L" (" + std::to_wstring(progress) + L"%)");
        }
    };
    
    // Use MPEG-TS format for better compatibility with players
    auto result = reclocker.ProcessHLSStream(narrow_url, "mpegts", progress_cb);
    
    if (result.success) {
        if (log_callback) {
            log_callback(L"HLS PTS correction applied successfully");
            if (result.discontinuities_detected > 0) {
                log_callback(L"Detected and corrected " + std::to_wstring(result.discontinuities_detected) + L" PTS discontinuities");
            }
        }
        
        AddDebugLog(L"HLS PTS reclock successful: " + std::to_wstring(result.discontinuities_detected) + 
                   L" discontinuities, " + std::to_wstring(result.timestamp_corrections) + L" corrections");
        
        // Return path to corrected stream file
        std::wstring wide_output(result.output_path.begin(), result.output_path.end());
        return wide_output;
    } else {
        if (log_callback) {
            std::wstring error_msg(result.error_message.begin(), result.error_message.end());
            log_callback(L"HLS PTS correction failed: " + error_msg + L", using original stream");
        }
        
        AddDebugLog(L"HLS PTS reclock failed: " + std::wstring(result.error_message.begin(), result.error_message.end()));
        return playlist_url; // Return original URL on failure
    }
}

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
    // Check if we should apply HLS PTS correction and prefer pipe streaming
    using namespace tardsplaya_hls_reclock;
    std::string narrow_url(playlist_url.begin(), playlist_url.end());
    
    if (integration_utils::ShouldApplyPTSCorrection(narrow_url)) {
        // Create reclock processor to check if tool is available
        auto config = integration_utils::CreateConfigForStreamType("live");
        config.verbose_logging = g_verboseDebug;
        TardsplayaHLSReclock reclocker(config);
        
        if (reclocker.IsReclockToolAvailable()) {
            if (log_callback) {
                log_callback(L"Using HLS PTS correction with direct pipe streaming for " + channel_name);
            }
            
            // Use direct pipe streaming approach
            return StartHLSReclockPipeThread(player_path, playlist_url, cancel_token, log_callback,
                                           channel_name, main_window, tab_index, player_process_handle);
        } else {
            if (log_callback) {
                log_callback(L"HLS PTS reclock tool not available, using standard streaming");
            }
        }
    }
    
    // Fallback: Apply HLS PTS discontinuity correction if needed (file-based approach)
    std::wstring processed_url = ProcessHLSWithPTSCorrection(playlist_url, channel_name, log_callback);
    
    // Check if we have a corrected local file instead of an HLS URL
    if (IsLocalFile(processed_url)) {
        if (log_callback) {
            log_callback(L"Using corrected stream file for direct playback");
        }
        return StartLocalFileStreamThread(player_path, processed_url, cancel_token, log_callback,
                                         channel_name, main_window, tab_index, player_process_handle);
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
        
        return StartTransportStreamThread(player_path, processed_url, cancel_token, log_callback,
                                         base_buffer_packets, channel_name, chunk_count, main_window, tab_index, player_process_handle);
    }
    
    // Use traditional HLS streaming (but check if it's a local file first)
    return std::thread([=, &cancel_token]() mutable {
        // If processed_url is a local file, we should have handled it above
        // This path is for original HLS URLs only
        if (IsLocalFile(processed_url)) {
            if (log_callback) {
                log_callback(L"Error: Local file reached traditional HLS path - this shouldn't happen");
            }
            return;
        }
        
        if (log_callback)
            log_callback(L"Streaming thread started (HLS fallback mode).");
        
        AddDebugLog(L"StartStreamThread: Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index) + 
                   L", BufferSegs=" + std::to_wstring(buffer_segments));
        
        bool ok = BufferAndPipeStreamToPlayer(player_path, processed_url, cancel_token, buffer_segments, channel_name, chunk_count, selected_quality, player_process_handle);
        
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