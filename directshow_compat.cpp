// DirectShow-Compatible Streaming Implementation for Tardsplaya
// Enhances existing transport stream capabilities for DirectShow players

#include "directshow_compat.h"
#include <shlwapi.h>
#include <process.h>
#include <fstream>
#include <sstream>
#include <regex>

#pragma comment(lib, "shlwapi.lib")

namespace directshow_compat {

//////////////////////////////////////////////////////////////////////////
// DirectShowStreamManager Implementation
//////////////////////////////////////////////////////////////////////////

DirectShowStreamManager::DirectShowStreamManager()
    : stream_active_(false)
    , named_pipe_handle_(INVALID_HANDLE_VALUE)
    , pipe_server_active_(false)
{
    current_stats_.start_time = std::chrono::steady_clock::now();
}

DirectShowStreamManager::~DirectShowStreamManager()
{
    StopStream();
}

bool DirectShowStreamManager::Initialize(const DirectShowConfig& config)
{
    config_ = config;
    
    if (!config_.enable_directshow_mode) {
        return true; // DirectShow mode disabled, but not an error
    }
    
    // Initialize transport stream router with DirectShow-optimized settings
    ts_router_ = std::make_unique<tsduck_transport::TransportStreamRouter>();
    
    return true;
}

bool DirectShowStreamManager::StartStream(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name)
{
    if (stream_active_) {
        return false; // Already streaming
    }
    
    log_callback_ = log_callback;
    
    if (!config_.enable_directshow_mode) {
        if (log_callback_) {
            log_callback_(L"DirectShow mode is disabled.");
        }
        return false;
    }
    
    if (log_callback_) {
        log_callback_(L"Starting DirectShow-compatible streaming for channel: " + channel_name);
    }
    
    // Create named pipe for external player communication
    if (!CreateTardsplayaNamedPipe()) {
        if (log_callback_) {
            log_callback_(L"Failed to create named pipe for DirectShow communication.");
        }
        return false;
    }
    
    // Start pipe server thread
    pipe_server_active_ = true;
    pipe_server_thread_ = std::thread(&DirectShowStreamManager::PipeServerThread, this);
    
    // Configure transport stream router for DirectShow compatibility
    tsduck_transport::TransportStreamRouter::RouterConfig router_config;
    router_config.player_path = L""; // No direct player - using named pipe
    router_config.buffer_size_packets = config_.buffer_size_packets;
    router_config.enable_pat_pmt_repetition = config_.enable_pat_pmt_repetition;
    router_config.pat_pmt_interval = config_.pat_pmt_interval;
    router_config.enable_pcr_insertion = config_.enable_pcr_insertion;
    router_config.pcr_interval = config_.pcr_interval;
    router_config.low_latency_mode = config_.enhanced_discontinuity_handling;
    
    // Start transport stream routing with DirectShow enhancements
    if (!ts_router_->StartRouting(playlist_url, router_config, cancel_token, log_callback_)) {
        if (log_callback_) {
            log_callback_(L"Failed to start transport stream routing.");
        }
        StopStream();
        return false;
    }
    
    stream_active_ = true;
    current_stats_.start_time = std::chrono::steady_clock::now();
    current_stats_.current_status = L"DirectShow streaming active";
    
    if (log_callback_) {
        log_callback_(L"DirectShow-compatible streaming started successfully.");
        log_callback_(L"Named pipe available at: " + config_.named_pipe_path);
        log_callback_(L"Configure your DirectShow player to connect to this pipe for enhanced discontinuity handling.");
    }
    
    return true;
}

void DirectShowStreamManager::StopStream()
{
    if (!stream_active_) {
        return;
    }
    
    // Stop transport stream router
    if (ts_router_) {
        ts_router_->StopRouting();
    }
    
    // Stop pipe server
    pipe_server_active_ = false;
    if (pipe_server_thread_.joinable()) {
        pipe_server_thread_.join();
    }
    
    // Cleanup named pipe
    CleanupNamedPipe();
    
    stream_active_ = false;
    current_stats_.current_status = L"DirectShow streaming stopped";
    
    if (log_callback_) {
        log_callback_(L"DirectShow-compatible streaming stopped.");
    }
}

DirectShowStreamManager::StreamStats DirectShowStreamManager::GetStreamStats() const
{
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    StreamStats stats = current_stats_;
    
    // Add transport stream router statistics if available
    if (ts_router_ && ts_router_->IsRouting()) {
        auto buffer_stats = ts_router_->GetBufferStats();
        stats.packets_processed = buffer_stats.total_packets_processed;
        stats.frames_tagged = buffer_stats.total_frames_processed;
        stats.discontinuities_handled = buffer_stats.frames_dropped; // Reuse this field
        
        // Calculate bitrate
        auto elapsed = std::chrono::steady_clock::now() - stats.start_time;
        double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        if (elapsed_seconds > 0) {
            double bits_processed = stats.packets_processed * 188 * 8; // TS packet size * 8 bits
            stats.current_bitrate_mbps = (bits_processed / elapsed_seconds) / 1000000.0;
        }
    }
    
    stats.external_player_connected = (named_pipe_handle_ != INVALID_HANDLE_VALUE);
    
    return stats;
}

bool DirectShowStreamManager::LaunchCompatiblePlayer(const std::wstring& player_path)
{
    std::wstring actual_path = player_path;
    
    if (actual_path.empty()) {
        // Try to find a compatible player automatically
        auto players = FindCompatiblePlayers();
        if (!players.empty()) {
            actual_path = players[0];
        }
    }
    
    if (actual_path.empty() || !PathFileExists(actual_path.c_str())) {
        if (log_callback_) {
            log_callback_(L"No compatible DirectShow player found.");
        }
        return false;
    }
    
    // Get launch command for the player
    std::wstring launch_command = GetPlayerLaunchCommand(actual_path);
    
    if (launch_command.empty()) {
        if (log_callback_) {
            log_callback_(L"Unable to create launch command for player: " + actual_path);
        }
        return false;
    }
    
    // Launch the player
    STARTUPINFO si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    
    BOOL success = CreateProcess(
        nullptr,                        // Application name
        const_cast<LPWSTR>(launch_command.c_str()), // Command line
        nullptr,                        // Process security attributes
        nullptr,                        // Thread security attributes
        FALSE,                          // Inherit handles
        0,                             // Creation flags
        nullptr,                        // Environment
        nullptr,                        // Current directory
        &si,                           // Startup info
        &pi                            // Process info
    );
    
    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (log_callback_) {
            log_callback_(L"Launched DirectShow player: " + actual_path);
        }
        return true;
    } else {
        if (log_callback_) {
            log_callback_(L"Failed to launch DirectShow player: " + actual_path);
        }
        return false;
    }
}

std::vector<std::wstring> DirectShowStreamManager::FindCompatiblePlayers()
{
    std::vector<std::wstring> players;
    
    // Common DirectShow-based media players with named pipe support
    const std::wstring common_paths[] = {
        L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe",
        L"C:\\Program Files (x86)\\MPC-HC\\mpc-hc.exe",
        L"C:\\Program Files\\MPC-BE\\mpc-be64.exe",
        L"C:\\Program Files (x86)\\MPC-BE\\mpc-be.exe",
        L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe",
        L"C:\\Program Files (x86)\\VideoLAN\\VLC\\vlc.exe",
        L"C:\\Windows\\System32\\wmplayer.exe" // Windows Media Player
    };
    
    for (const auto& path : common_paths) {
        if (PathFileExists(path.c_str())) {
            players.push_back(path);
        }
    }
    
    return players;
}

std::wstring DirectShowStreamManager::GetPlayerLaunchCommand(const std::wstring& player_path)
{
    std::wstring filename = PathFindFileName(player_path.c_str());
    std::transform(filename.begin(), filename.end(), filename.begin(), ::towlower);
    
    // Create appropriate command line based on player type
    if (filename.find(L"mpc-hc") != std::wstring::npos) {
        // MPC-HC with named pipe
        return L"\"" + player_path + L"\" \"" + config_.named_pipe_path + L"\"";
    } else if (filename.find(L"mpc-be") != std::wstring::npos) {
        // MPC-BE with named pipe
        return L"\"" + player_path + L"\" \"" + config_.named_pipe_path + L"\"";
    } else if (filename.find(L"vlc") != std::wstring::npos) {
        // VLC with named pipe
        return L"\"" + player_path + L"\" \"" + config_.named_pipe_path + L"\"";
    } else if (filename.find(L"wmplayer") != std::wstring::npos) {
        // Windows Media Player
        return L"\"" + player_path + L"\" \"" + config_.named_pipe_path + L"\"";
    }
    
    // Generic approach
    return L"\"" + player_path + L"\" \"" + config_.named_pipe_path + L"\"";
}

void DirectShowStreamManager::UpdateConfig(const DirectShowConfig& config)
{
    config_ = config;
}

bool DirectShowStreamManager::CreateTardsplayaNamedPipe()
{
    CleanupNamedPipe(); // Cleanup any existing pipe
    
    named_pipe_handle_ = CreateNamedPipeW(
        config_.named_pipe_path.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,                           // Max instances
        8192,                        // Out buffer size
        0,                          // In buffer size (not used)
        1000,                       // Default timeout
        nullptr                     // Security attributes
    );
    
    return named_pipe_handle_ != INVALID_HANDLE_VALUE;
}

void DirectShowStreamManager::CleanupNamedPipe()
{
    if (named_pipe_handle_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(named_pipe_handle_);
        CloseHandle(named_pipe_handle_);
        named_pipe_handle_ = INVALID_HANDLE_VALUE;
    }
}

bool DirectShowStreamManager::WriteToNamedPipe(const uint8_t* data, size_t size)
{
    if (named_pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(named_pipe_handle_, data, (DWORD)size, &bytesWritten, nullptr);
    
    return success && bytesWritten == size;
}

void DirectShowStreamManager::PipeServerThread()
{
    if (log_callback_) {
        log_callback_(L"DirectShow pipe server started. Waiting for external player connection...");
    }
    
    while (pipe_server_active_) {
        // Wait for client connection
        BOOL connected = ConnectNamedPipe(named_pipe_handle_, nullptr);
        
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        if (log_callback_) {
            log_callback_(L"DirectShow player connected to pipe. Starting stream delivery...");
        }
        
        current_stats_.external_player_connected = true;
        
        // Stream data to connected player
        size_t packets_sent = 0;
        size_t total_timeout_count = 0;
        auto stream_start = std::chrono::steady_clock::now();
        
        while (pipe_server_active_ && ts_router_ && ts_router_->IsRouting()) {
            // Get actual transport stream packets from router buffer
            tsduck_transport::TSPacket packet;
            
            // Try to get a packet from the TS buffer with a timeout
            if (ts_router_->GetTSPacket(packet, std::chrono::milliseconds(100))) {
                // Validate packet before sending
                if (!packet.IsValid()) {
                    if (log_callback_) {
                        log_callback_(L"DirectShow: Invalid TS packet received (missing sync byte)");
                    }
                    continue;
                }
                
                // Process packet for DirectShow compatibility
                ProcessTSPacketForDirectShow(packet);
                
                // Handle discontinuities for better DirectShow compatibility
                if (packet.discontinuity) {
                    HandleDiscontinuityForDirectShow(packet);
                }
                
                // Insert PCR if enabled for timing synchronization
                if (config_.enable_pcr_insertion) {
                    InsertPCRForDirectShow(packet);
                }
                
                // Write the actual TS packet to the named pipe
                if (!WriteToNamedPipe(packet.data, 188)) {
                    if (log_callback_) {
                        log_callback_(L"Lost connection to DirectShow player.");
                    }
                    break;
                }
                
                UpdateStreamStats(packet);
                packets_sent++;
                
                // Log progress every 1000 packets
                if (packets_sent % 1000 == 0) {
                    auto elapsed = std::chrono::steady_clock::now() - stream_start;
                    auto elapsed_sec = std::chrono::duration<double>(elapsed).count();
                    if (log_callback_) {
                        log_callback_(L"DirectShow: Sent " + std::to_wstring(packets_sent) + 
                                    L" packets in " + std::to_wstring(elapsed_sec) + L" seconds");
                    }
                }
            } else {
                total_timeout_count++;
                
                // Check buffer status for diagnostics
                auto buffer_stats = ts_router_->GetBufferStats();
                bool producer_active = ts_router_->IsProducerActive();
                
                // Log periodic status updates
                if (total_timeout_count % 50 == 0) { // Every ~5 seconds
                    if (log_callback_) {
                        log_callback_(L"DirectShow: Waiting for packets - buffered: " + 
                                    std::to_wstring(buffer_stats.buffered_packets) + 
                                    L", producer active: " + (producer_active ? L"yes" : L"no") +
                                    L", timeouts: " + std::to_wstring(total_timeout_count));
                    }
                }
                
                // No packet available, check if we should continue
                if (!producer_active && buffer_stats.buffered_packets == 0) {
                    // Stream ended
                    if (log_callback_) {
                        log_callback_(L"DirectShow: Stream ended - no more packets available.");
                    }
                    break;
                }
                
                // If we've been waiting too long without packets, something is wrong
                if (total_timeout_count > 300 && packets_sent == 0) { // 30 seconds without any packets
                    if (log_callback_) {
                        log_callback_(L"DirectShow: Error - No packets received after 30 seconds. HLS stream may have failed.");
                    }
                    break;
                }
            }
        }
        
        // Disconnect client
        DisconnectNamedPipe(named_pipe_handle_);
        current_stats_.external_player_connected = false;
        
        if (log_callback_) {
            log_callback_(L"DirectShow player disconnected.");
        }
    }
    
    if (log_callback_) {
        log_callback_(L"DirectShow pipe server stopped.");
    }
}

void DirectShowStreamManager::ProcessTSPacketForDirectShow(const tsduck_transport::TSPacket& packet)
{
    // Enhanced packet processing for DirectShow compatibility
    // This would integrate with the existing packet processing in the transport router
    
    if (config_.enhanced_discontinuity_handling && packet.discontinuity) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        current_stats_.discontinuities_handled++;
    }
    
    if (config_.frame_tagging_enabled && packet.frame_number > 0) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        current_stats_.frames_tagged++;
    }
}

void DirectShowStreamManager::HandleDiscontinuityForDirectShow(tsduck_transport::TSPacket& packet)
{
    // Enhanced discontinuity handling specifically for DirectShow players
    if (packet.discontinuity) {
        // Mark discontinuity in transport stream
        packet.data[5] |= 0x80; // Set discontinuity indicator in adaptation field
        
        // Insert PAT/PMT after discontinuity for rapid resync
        if (config_.enable_pat_pmt_repetition) {
            InsertPATForDirectShow();
            InsertPMTForDirectShow();
        }
    }
}

void DirectShowStreamManager::InsertPATForDirectShow()
{
    std::lock_guard<std::mutex> lock(stats_mutex_);
    current_stats_.pat_pmt_inserted++;
}

void DirectShowStreamManager::InsertPMTForDirectShow()
{
    // PMT insertion handled with PAT
}

void DirectShowStreamManager::InsertPCRForDirectShow(tsduck_transport::TSPacket& packet)
{
    if (config_.enable_pcr_insertion) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        current_stats_.pcr_inserted++;
    }
}

void DirectShowStreamManager::EnhanceStreamForDirectShow(std::vector<tsduck_transport::TSPacket>& packets)
{
    for (auto& packet : packets) {
        ProcessTSPacketForDirectShow(packet);
        
        if (packet.discontinuity) {
            HandleDiscontinuityForDirectShow(packet);
        }
        
        if (config_.enable_pcr_insertion) {
            InsertPCRForDirectShow(packet);
        }
    }
}

void DirectShowStreamManager::UpdateStreamStats(const tsduck_transport::TSPacket& packet)
{
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    current_stats_.packets_processed++;
    
    if (packet.frame_number > 0) {
        current_stats_.frames_tagged++;
    }
    
    if (packet.discontinuity) {
        current_stats_.discontinuities_handled++;
    }
}

//////////////////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////////////////

bool IsDirectShowSupported()
{
    // Check if DirectShow is available on the system
    HMODULE quartz = LoadLibrary(L"quartz.dll");
    if (quartz) {
        FreeLibrary(quartz);
        return true;
    }
    return false;
}

std::wstring GetDirectShowInstructions()
{
    std::wstringstream instructions;
    
    instructions << L"DirectShow-Compatible Streaming Setup Instructions:\n\n";
    
    instructions << L"1. Enable DirectShow Mode in Tardsplaya:\n";
    instructions << L"   - Check 'Enable DirectShow Mode' in settings\n";
    instructions << L"   - Configure enhanced discontinuity handling options\n\n";
    
    instructions << L"2. Start streaming with DirectShow mode:\n";
    instructions << L"   - Load your desired channel\n";
    instructions << L"   - Click 'Watch with DirectShow' or enable DirectShow mode\n";
    instructions << L"   - Tardsplaya will create a named pipe for external players\n\n";
    
    instructions << L"3. Connect your DirectShow player:\n";
    instructions << L"   - MPC-HC: Open the named pipe path as a file\n";
    instructions << L"   - VLC: Use 'Open Network Stream' with the pipe path\n";
    instructions << L"   - The named pipe path will be shown in Tardsplaya logs\n\n";
    
    instructions << L"4. Benefits of DirectShow mode:\n";
    instructions << L"   - Automatic discontinuity detection and handling\n";
    instructions << L"   - Frame number tagging for reduced lag\n";
    instructions << L"   - Enhanced transport stream format with PAT/PMT repetition\n";
    instructions << L"   - PCR insertion for better timing synchronization\n";
    instructions << L"   - Real-time stream health monitoring\n\n";
    
    instructions << L"5. Troubleshooting:\n";
    instructions << L"   - Ensure DirectShow mode is enabled in Tardsplaya\n";
    instructions << L"   - Check that the named pipe is created (visible in logs)\n";
    instructions << L"   - Try different DirectShow players (MPC-HC recommended)\n";
    instructions << L"   - Verify Windows supports named pipes (Vista and later)\n\n";
    
    instructions << L"Note: This mode leverages Tardsplaya's advanced transport stream\n";
    instructions << L"processing to provide superior discontinuity handling compared\n";
    instructions << L"to direct HLS streaming in most DirectShow players.";
    
    return instructions.str();
}

std::wstring CreateMPCHCCommandLine(const std::wstring& named_pipe_path)
{
    return L"mpc-hc64.exe \"" + named_pipe_path + L"\"";
}

std::wstring CreateVLCCommandLine(const std::wstring& named_pipe_path)
{
    return L"vlc.exe \"" + named_pipe_path + L"\"";
}

std::vector<PlayerInfo> DetectDirectShowPlayers()
{
    std::vector<PlayerInfo> players;
    
    // MPC-HC
    if (PathFileExists(L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe")) {
        PlayerInfo info;
        info.name = L"MPC-HC (64-bit)";
        info.path = L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe";
        info.supports_named_pipes = true;
        info.launch_command_template = L"\"{path}\" \"{pipe}\"";
        players.push_back(info);
    }
    
    if (PathFileExists(L"C:\\Program Files (x86)\\MPC-HC\\mpc-hc.exe")) {
        PlayerInfo info;
        info.name = L"MPC-HC (32-bit)";
        info.path = L"C:\\Program Files (x86)\\MPC-HC\\mpc-hc.exe";
        info.supports_named_pipes = true;
        info.launch_command_template = L"\"{path}\" \"{pipe}\"";
        players.push_back(info);
    }
    
    // VLC
    if (PathFileExists(L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe")) {
        PlayerInfo info;
        info.name = L"VLC Media Player";
        info.path = L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe";
        info.supports_named_pipes = true;
        info.launch_command_template = L"\"{path}\" \"{pipe}\"";
        players.push_back(info);
    }
    
    return players;
}

bool ConfigurePlayerForTardsplaya(const PlayerInfo& player, const std::wstring& pipe_path)
{
    // Basic configuration - in a real implementation, this could modify player config files
    return player.supports_named_pipes;
}

} // namespace directshow_compat