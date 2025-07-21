#pragma once
// DirectShow-Compatible Streaming Mode for Tardsplaya
// Provides enhanced discontinuity handling that DirectShow players can leverage

#include <windows.h>
#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include "tsduck_transport_router.h"

namespace directshow_compat {

// DirectShow-compatible streaming configuration
struct DirectShowConfig {
    bool enable_directshow_mode = false;     // Enable DirectShow-compatible output
    bool auto_register_filter = true;        // Automatically register filter if available
    std::wstring named_pipe_path = L"\\\\.\\pipe\\TardsplayaStream"; // Named pipe for communication
    bool enhanced_discontinuity_handling = true; // Use advanced discontinuity detection
    bool frame_tagging_enabled = true;       // Enable frame number tagging
    size_t buffer_size_packets = 8000;       // Buffer size for DirectShow output
    std::wstring preferred_player_path;      // Preferred DirectShow player path
    
    // Advanced options
    bool enable_pat_pmt_repetition = true;   // Repeat PAT/PMT for DirectShow compatibility
    std::chrono::milliseconds pat_pmt_interval{100}; // PAT/PMT repetition interval
    bool enable_pcr_insertion = true;        // Insert PCR for better timing
    std::chrono::milliseconds pcr_interval{40}; // PCR insertion interval
};

// DirectShow-compatible stream manager
class DirectShowStreamManager {
public:
    DirectShowStreamManager();
    ~DirectShowStreamManager();
    
    // Initialize DirectShow-compatible streaming
    bool Initialize(const DirectShowConfig& config);
    
    // Start DirectShow-compatible streaming for a channel
    bool StartStream(
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        std::function<void(const std::wstring&)> log_callback = nullptr,
        const std::wstring& channel_name = L""
    );
    
    // Stop DirectShow-compatible streaming
    void StopStream();
    
    // Check if DirectShow streaming is active
    bool IsActive() const { return stream_active_; }
    
    // Get stream statistics
    struct StreamStats {
        uint64_t packets_processed = 0;
        uint64_t discontinuities_handled = 0;
        uint64_t frames_tagged = 0;
        uint64_t pat_pmt_inserted = 0;
        uint64_t pcr_inserted = 0;
        bool external_player_connected = false;
        std::chrono::steady_clock::time_point start_time;
        double current_bitrate_mbps = 0.0;
        std::wstring current_status;
    };
    StreamStats GetStreamStats() const;
    
    // Get named pipe path for external players
    std::wstring GetNamedPipePath() const { return config_.named_pipe_path; }
    
    // Helper functions for external players
    bool LaunchCompatiblePlayer(const std::wstring& player_path = L"");
    std::vector<std::wstring> FindCompatiblePlayers();
    std::wstring GetPlayerLaunchCommand(const std::wstring& player_path);
    
    // Configuration management
    void UpdateConfig(const DirectShowConfig& config);
    DirectShowConfig GetConfig() const { return config_; }
    
private:
    DirectShowConfig config_;
    std::unique_ptr<tsduck_transport::TransportStreamRouter> ts_router_;
    std::atomic<bool> stream_active_;
    std::function<void(const std::wstring&)> log_callback_;
    
    // Statistics tracking
    mutable std::mutex stats_mutex_;
    StreamStats current_stats_;
    
    // Named pipe for external player communication
    HANDLE named_pipe_handle_;
    std::thread pipe_server_thread_;
    std::atomic<bool> pipe_server_active_;
    
    // Stream processing
    void PipeServerThread();
    bool CreateTardsplayaNamedPipe();
    void CleanupNamedPipe();
    bool WriteToNamedPipe(const uint8_t* data, size_t size);
    
    // DirectShow-compatible packet processing
    void ProcessTSPacketForDirectShow(const tsduck_transport::TSPacket& packet);
    void HandleDiscontinuityForDirectShow(tsduck_transport::TSPacket& packet);
    void InsertPATForDirectShow();
    void InsertPMTForDirectShow();
    void InsertPCRForDirectShow(tsduck_transport::TSPacket& packet);
    
    // Stream enhancement
    void EnhanceStreamForDirectShow(std::vector<tsduck_transport::TSPacket>& packets);
    
    // Update statistics
    void UpdateStreamStats(const tsduck_transport::TSPacket& packet);
};

// Helper functions for DirectShow integration

// Check if system supports DirectShow
bool IsDirectShowSupported();

// Get DirectShow player configuration instructions
std::wstring GetDirectShowInstructions();

// Create MPC-HC command line for Tardsplaya stream
std::wstring CreateMPCHCCommandLine(const std::wstring& named_pipe_path);

// Create VLC command line for Tardsplaya stream
std::wstring CreateVLCCommandLine(const std::wstring& named_pipe_path);

// Auto-detect and configure DirectShow players
struct PlayerInfo {
    std::wstring name;
    std::wstring path;
    std::wstring version;
    bool supports_named_pipes;
    std::wstring launch_command_template;
};

std::vector<PlayerInfo> DetectDirectShowPlayers();
bool ConfigurePlayerForTardsplaya(const PlayerInfo& player, const std::wstring& pipe_path);

} // namespace directshow_compat