#pragma once
// TSDuck-inspired Transport Stream Router for Tardsplaya
// Implements transport stream re-routing functionality to buffer streams to media players

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <chrono>
#include <functional>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tsduck_transport {

    // Transport Stream packet size (MPEG-TS standard)
    static constexpr size_t TS_PACKET_SIZE = 188;
    
    // Transport Stream packet header structure
    struct TSPacket {
        uint8_t data[TS_PACKET_SIZE];
        std::chrono::steady_clock::time_point timestamp;
        uint16_t pid = 0;
        bool payload_unit_start = false;
        bool discontinuity = false;
        
        TSPacket() {
            memset(data, 0, TS_PACKET_SIZE);
            timestamp = std::chrono::steady_clock::now();
        }
        
        // Parse packet header information
        void ParseHeader();
        
        // Check if this is a valid TS packet
        bool IsValid() const { return data[0] == 0x47; }
    };
    
    // Transport Stream buffer for smooth re-routing
    class TSBuffer {
    public:
        TSBuffer(size_t max_packets = 25000); // ~4.7MB buffer for smooth multi-stream streaming
        
        // Add TS packet to buffer
        bool AddPacket(const TSPacket& packet);
        
        // Get next packet from buffer (blocking until available)
        bool GetNextPacket(TSPacket& packet, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
        
        // Get buffer status
        size_t GetBufferedPackets() const;
        bool IsEmpty() const;
        bool IsFull() const;
        
        // Clear buffer (for discontinuities)
        void Clear();
        
    private:
        mutable std::mutex mutex_;
        std::queue<TSPacket> packet_queue_;
        size_t max_packets_;
        std::atomic<bool> producer_active_{true};
    };
    
    // HLS to Transport Stream converter
    class HLSToTSConverter {
    public:
        HLSToTSConverter();
        
        // Convert HLS segment data to TS packets
        std::vector<TSPacket> ConvertSegment(const std::vector<uint8_t>& hls_data, bool is_first_segment = false);
        
        // Set conversion parameters
        void SetProgramID(uint16_t program_id) { program_id_ = program_id; }
        void SetPMTPID(uint16_t pmt_pid) { pmt_pid_ = pmt_pid; }
        
        // Reset converter state for new stream
        void Reset();
        
    private:
        uint16_t program_id_ = 1;
        uint16_t pmt_pid_ = 0x1000;
        uint16_t video_pid_ = 0x1001;
        uint16_t audio_pid_ = 0x1002;
        uint8_t continuity_counter_ = 0;
        bool pat_sent_ = false;
        bool pmt_sent_ = false;
        
        // Generate PAT (Program Association Table)
        TSPacket GeneratePAT();
        
        // Generate PMT (Program Map Table)
        TSPacket GeneratePMT();
        
        // Wrap data in TS packets
        std::vector<TSPacket> WrapDataInTS(const uint8_t* data, size_t size, uint16_t pid, bool payload_start = false);
        
        // Calculate CRC32 for PSI tables
        uint32_t CalculateCRC32(const uint8_t* data, size_t length);
    };
    
    // Transport Stream Router - main component for re-routing streams to media players
    class TransportStreamRouter {
    public:
        TransportStreamRouter();
        ~TransportStreamRouter();
        
        // Router configuration
        struct RouterConfig {
            std::wstring player_path = L"mpv.exe";
            std::wstring player_args = L"-";  // Read from stdin
            size_t buffer_size_packets = 15000;  // ~2.8MB buffer for reduced frame drops
            bool enable_adaptation_field = true;
            bool enable_pcr_insertion = true;
            std::chrono::milliseconds pcr_interval{40}; // PCR every 40ms
            bool enable_pat_pmt_repetition = true;
            std::chrono::milliseconds pat_pmt_interval{100}; // PAT/PMT every 100ms
        };
        
        // Start routing HLS stream to media player via transport stream
        bool StartRouting(const std::wstring& hls_playlist_url, 
                         const RouterConfig& config,
                         std::atomic<bool>& cancel_token,
                         std::function<void(const std::wstring&)> log_callback = nullptr);
        
        // Stop routing
        void StopRouting();
        
        // Get routing status
        bool IsRouting() const { return routing_active_; }
        
        // Get buffer statistics
        struct BufferStats {
            size_t buffered_packets = 0;
            size_t total_packets_processed = 0;
            double buffer_utilization = 0.0; // 0.0 to 1.0
            std::chrono::milliseconds avg_packet_interval{0};
        };
        BufferStats GetBufferStats() const;
        
        // Get player process handle for external monitoring
        HANDLE GetPlayerProcessHandle() const { return player_process_handle_; }
        
    private:
        std::unique_ptr<TSBuffer> ts_buffer_;
        std::unique_ptr<HLSToTSConverter> hls_converter_;
        std::atomic<bool> routing_active_{false};
        std::thread hls_fetcher_thread_;
        std::thread ts_router_thread_;
        std::atomic<size_t> total_packets_processed_{0};
        mutable std::mutex stats_mutex_;
        
        RouterConfig current_config_;
        std::function<void(const std::wstring&)> log_callback_;
        HANDLE player_process_handle_;
        
        // HLS fetching thread - downloads segments and converts to TS
        void HLSFetcherThread(const std::wstring& playlist_url, std::atomic<bool>& cancel_token);
        
        // TS routing thread - sends TS packets to media player
        void TSRouterThread(std::atomic<bool>& cancel_token);
        
        // Launch media player process with transport stream input
        bool LaunchMediaPlayer(const RouterConfig& config, HANDLE& process_handle, HANDLE& stdin_handle);
        
        // Send TS packet to media player
        bool SendTSPacketToPlayer(HANDLE stdin_handle, const TSPacket& packet);
        
        // Fetch HLS segment data
        bool FetchHLSSegment(const std::wstring& segment_url, std::vector<uint8_t>& data, std::atomic<bool>* cancel_token = nullptr);
        
        // Parse HLS playlist for segment URLs
        std::vector<std::wstring> ParseHLSPlaylist(const std::string& playlist_content, const std::wstring& base_url);
        
        // Insert PCR (Program Clock Reference) for timing
        void InsertPCR(TSPacket& packet, uint64_t pcr_value);
    };
    

    
} // namespace tsduck_transport