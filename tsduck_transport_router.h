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
        
        // Stream type identification
        bool is_video_packet = false;     // True if this packet contains video data
        bool is_audio_packet = false;     // True if this packet contains audio data
        
        // PTS/DTS timing information (extracted from payload)
        int64_t pts = -1;                 // Presentation timestamp (in 90kHz clock)
        int64_t dts = -1;                 // Decode timestamp (in 90kHz clock)
        bool has_pts = false;             // True if PTS is valid
        bool has_dts = false;             // True if DTS is valid
        
        // Frame Number Tagging for lag reduction
        uint64_t frame_number = 0;        // Global frame sequence number
        uint32_t segment_frame_number = 0; // Frame number within current segment
        bool is_key_frame = false;        // Indicates if this is a key/I-frame
        std::chrono::milliseconds frame_duration{0}; // Expected frame duration for timing
        
        // Video synchronization data
        uint64_t video_frame_number = 0;  // Video-specific frame counter
        bool video_sync_lost = false;     // True if video synchronization is lost
        
        TSPacket() {
            memset(data, 0, TS_PACKET_SIZE);
            timestamp = std::chrono::steady_clock::now();
        }
        
        // Parse packet header information
        void ParseHeader();
        
        // Parse PTS/DTS from PES header if present
        void ParsePTSDTS();
        
        // Apply timestamp discontinuity correction
        void ApplyTimestampCorrection(int64_t pts_offset, int64_t dts_offset);
        
        // Check if this is a valid TS packet
        bool IsValid() const { return data[0] == 0x47; }
        
        // Frame Number Tagging methods
        void SetFrameInfo(uint64_t global_frame, uint32_t segment_frame, bool key_frame = false, std::chrono::milliseconds duration = std::chrono::milliseconds(0));
        void SetVideoInfo(uint64_t video_frame, bool is_video, bool is_audio = false);
        std::wstring GetFrameDebugInfo() const;
        bool IsFrameDropDetected(const TSPacket& previous_packet) const;
        bool IsVideoSyncValid() const;
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
        
        // Reset buffer state for new stream
        void Reset();
        
        // Signal end of stream (no more packets will be added)
        void SignalEndOfStream();
        
        // Check if producer is still active
        bool IsProducerActive() const { return producer_active_.load(); }
        
        // Enable low-latency mode for more aggressive packet dropping
        void SetLowLatencyMode(bool enabled) { low_latency_mode_ = enabled; }
        
    private:
        mutable std::mutex mutex_;
        std::queue<TSPacket> packet_queue_;
        size_t max_packets_;
        std::atomic<bool> producer_active_{true};
        bool low_latency_mode_{false};
    };
    
    // HLS to Transport Stream converter with PTS discontinuity handling
    class HLSToTSConverter {
    public:
        HLSToTSConverter();
        
        // Convert HLS segment data to TS packets
        std::vector<TSPacket> ConvertSegment(const std::vector<uint8_t>& hls_data, bool is_first_segment = false);
        
        // Set conversion parameters
        void SetProgramID(uint16_t program_id) { program_id_ = program_id; }
        void SetPMTPID(uint16_t pmt_pid) { pmt_pid_ = pmt_pid; }
        
        // Enable/disable PTS discontinuity correction
        void SetPTSDiscontinuityCorrection(bool enable) { pts_correction_enabled_ = enable; }
        void SetDiscontinuityThreshold(int64_t threshold_ms) { discontinuity_threshold_ = threshold_ms * 90; } // Convert ms to 90kHz
        
        // Set log callback for discontinuity messages
        void SetLogCallback(std::function<void(const std::wstring&)> log_callback) { log_callback_ = log_callback; }
        
        // Reset converter state for new stream
        void Reset();
        
        // Reset PTS/DTS correction state (called when SCTE-35 discontinuity is detected)
        void ResetDiscontinuityState();
        
    private:
        uint16_t program_id_ = 1;
        uint16_t pmt_pid_ = 0x1000;
        uint16_t video_pid_ = 0x1001;
        uint16_t audio_pid_ = 0x1002;
        uint8_t continuity_counter_ = 0;
        bool pat_sent_ = false;
        bool pmt_sent_ = false;
        
        // PTS/DTS discontinuity correction state
        bool pts_correction_enabled_ = true;
        int64_t discontinuity_threshold_ = 1800000; // 20 seconds in 90kHz (20 * 1000 * 90)
        int64_t last_video_pts_ = -1;
        int64_t last_video_dts_ = -1;
        int64_t last_audio_pts_ = -1;
        int64_t pts_offset_ = 0;
        int64_t dts_offset_ = 0;
        bool discontinuity_detected_ = false;
        
        // Frame Number Tagging state
        uint64_t global_frame_counter_ = 0;     // Total frames processed across all segments
        uint32_t segment_frame_counter_ = 0;    // Frames in current segment
        std::chrono::steady_clock::time_point last_frame_time_;
        std::chrono::milliseconds estimated_frame_duration_{33}; // Default ~30fps
        
        // Stream type detection state
        uint16_t detected_video_pid_ = 0;
        uint16_t detected_audio_pid_ = 0;
        
        // Logging callback
        std::function<void(const std::wstring&)> log_callback_;
        
        // Generate PAT (Program Association Table)
        TSPacket GeneratePAT();
        
        // Generate PMT (Program Map Table)
        TSPacket GeneratePMT();
        
        // Wrap data in TS packets
        std::vector<TSPacket> WrapDataInTS(const uint8_t* data, size_t size, uint16_t pid, bool payload_start = false);
        
        // Calculate CRC32 for PSI tables
        uint32_t CalculateCRC32(const uint8_t* data, size_t length);
        
        // Detect and classify stream types (video/audio)
        void DetectStreamTypes(TSPacket& packet);
        
        // PTS/DTS discontinuity correction methods
        void CheckAndCorrectDiscontinuity(TSPacket& packet);
        void ApplyPTSCorrection(std::vector<TSPacket>& packets);
        int64_t ExtractPTSFromPES(const uint8_t* pes_data, size_t pes_length);
        int64_t ExtractDTSFromPES(const uint8_t* pes_data, size_t pes_length);
        void WritePTSToPES(uint8_t* pes_data, int64_t pts);
        void WriteDTSToPES(uint8_t* pes_data, int64_t dts);
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
            
            // PTS discontinuity correction settings
            bool enable_pts_discontinuity_correction = true;
            int64_t discontinuity_threshold_ms = 20000; // 20 seconds threshold
            
            // Low-latency streaming optimizations
            bool low_latency_mode = true;  // Enable aggressive latency reduction
            size_t max_segments_to_buffer = 2;  // Only buffer latest N segments for live edge
            std::chrono::milliseconds playlist_refresh_interval{500}; // Check for new segments every 500ms
            bool skip_old_segments = true;  // Skip older segments when catching up
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
            
            // Frame Number Tagging statistics
            uint64_t total_frames_processed = 0;
            uint32_t frames_dropped = 0;
            uint32_t frames_duplicated = 0;
            double current_fps = 0.0;
            std::chrono::milliseconds avg_frame_interval{0};
            
            // Video/Audio stream health statistics
            uint64_t video_packets_processed = 0;
            uint64_t audio_packets_processed = 0;
            uint64_t video_frames_processed = 0;
            uint32_t video_sync_loss_count = 0;
            bool video_stream_healthy = true;
            bool audio_stream_healthy = true;
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
        
        // Frame Number Tagging statistics
        std::atomic<uint64_t> total_frames_processed_{0};
        std::atomic<uint32_t> frames_dropped_{0};
        std::atomic<uint32_t> frames_duplicated_{0};
        std::atomic<uint64_t> last_frame_number_{0};
        std::chrono::steady_clock::time_point last_frame_time_;
        std::chrono::steady_clock::time_point stream_start_time_;
        
        // Video/Audio stream health tracking
        std::atomic<uint64_t> video_packets_processed_{0};
        std::atomic<uint64_t> audio_packets_processed_{0};
        std::atomic<uint64_t> video_frames_processed_{0};
        std::atomic<uint64_t> last_video_frame_number_{0};
        std::atomic<uint32_t> video_sync_loss_count_{0};
        std::chrono::steady_clock::time_point last_video_packet_time_;
        std::chrono::steady_clock::time_point last_audio_packet_time_;
        
        // HLS fetching thread - downloads segments and converts to TS
        void HLSFetcherThread(const std::wstring& playlist_url, std::atomic<bool>& cancel_token);
        
        // TS routing thread - sends TS packets to media player
        void TSRouterThread(std::atomic<bool>& cancel_token);
        
        // Reset frame statistics (for discontinuities)
        void ResetFrameStatistics();
        
        // Video stream health monitoring
        bool IsVideoStreamHealthy() const;
        bool IsAudioStreamHealthy() const;
        void CheckStreamHealth(const TSPacket& packet);
        
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