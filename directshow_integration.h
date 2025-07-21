#pragma once
// Tardsplaya DirectShow Filter Integration
// Provides communication between main Tardsplaya app and DirectShow filter

#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include "tsduck_transport_router.h"

namespace tardsplaya_filter {

// Communication structure (must match filter implementation)
struct FilterData {
    tsduck_transport::TSPacket packet;
    DWORD stream_id;
    LONGLONG timestamp;
    bool discontinuity_detected;
    bool end_of_stream;
    
    FilterData() : stream_id(0), timestamp(0), discontinuity_detected(false), end_of_stream(false) {}
};

// DirectShow Filter Communication Manager
class FilterCommunication {
public:
    FilterCommunication();
    ~FilterCommunication();
    
    // Initialize communication to DirectShow filter
    bool Initialize(const std::wstring& pipe_name = L"\\\\.\\pipe\\TardsplayaFilter");
    
    // Cleanup communication
    void Cleanup();
    
    // Connect to DirectShow filter
    bool ConnectToFilter(DWORD timeout_ms = 5000);
    
    // Send packet data to DirectShow filter
    bool SendPacketData(const FilterData& data, DWORD timeout_ms = 1000);
    
    // Signal end of stream to filter
    bool SendEndOfStream();
    
    // Check if connected
    bool IsConnected() const { return pipe_connected_; }
    
    // Get filter registration status
    static bool IsFilterRegistered();
    
    // Register/unregister filter
    static bool RegisterFilter();
    static bool UnregisterFilter();
    
private:
    HANDLE pipe_handle_;
    std::atomic<bool> pipe_connected_;
    std::wstring pipe_name_;
};

// DirectShow Filter Stream Manager
// Integrates with existing stream_thread.cpp to provide DirectShow output
class FilterStreamManager {
public:
    FilterStreamManager();
    ~FilterStreamManager();
    
    // Start DirectShow filter streaming
    bool StartFilterStream(
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        std::function<void(const std::wstring&)> log_callback = nullptr,
        const std::wstring& channel_name = L""
    );
    
    // Stop DirectShow filter streaming
    void StopFilterStream();
    
    // Check if filter streaming is active
    bool IsFilterStreamActive() const { return filter_active_; }
    
    // Get filter communication handle for MPC-HC discovery
    std::wstring GetFilterPipeName() const { return pipe_name_; }
    
    // Get stream statistics
    struct FilterStats {
        uint64_t packets_sent = 0;
        uint64_t discontinuities_handled = 0;
        uint64_t frames_processed = 0;
        bool filter_connected = false;
        std::chrono::steady_clock::time_point start_time;
    };
    FilterStats GetFilterStats() const;
    
private:
    std::unique_ptr<FilterCommunication> filter_comm_;
    std::unique_ptr<tsduck_transport::TransportStreamRouter> ts_router_;
    std::thread filter_thread_;
    std::atomic<bool> filter_active_;
    std::atomic<bool> stop_requested_;
    
    std::wstring pipe_name_;
    std::function<void(const std::wstring&)> log_callback_;
    
    // Filter streaming thread
    void FilterStreamThread(
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        const std::wstring& channel_name
    );
    
    // Convert transport stream packets to filter data
    FilterData ConvertTSPacketToFilterData(const tsduck_transport::TSPacket& packet, DWORD stream_id);
    
    // Handle discontinuities in the stream
    void HandleDiscontinuity(FilterData& data);
    
    // Statistics tracking
    mutable std::mutex stats_mutex_;
    FilterStats current_stats_;
    void UpdateStats(const FilterData& data);
};

// Helper functions for DirectShow filter integration

// Check if MPC-HC or other DirectShow players can use our filter
bool IsDirectShowPlayerCompatible();

// Get list of DirectShow-based media players that could use our filter
std::vector<std::wstring> GetCompatibleDirectShowPlayers();

// Launch MPC-HC with our DirectShow filter as source
bool LaunchMPCHCWithFilter(const std::wstring& mpc_path = L"");

// Instructions for user to configure MPC-HC to use our filter
std::wstring GetMPCHCConfigurationInstructions();

} // namespace tardsplaya_filter