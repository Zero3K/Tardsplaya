#pragma once
// HLS Discontinuity Handler - Converts HLS streams with discontinuity markers into continuous data
// Primarily handles #EXT-X-DISCONTINUITY markers for seamless stream transitions
// Based on concepts from https://github.com/SnailTowardThesun/hls-decode

#include <string>
#include <vector>
#include <map>
#include <queue>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace hls_discontinuity {

    // HLS segment with discontinuity information
    struct SegmentInfo {
        std::wstring url;
        std::chrono::milliseconds duration{0};
        bool has_discontinuity = false;
        bool has_scte35_out = false;  // Ad start marker
        bool has_scte35_in = false;   // Ad end marker
        uint64_t sequence_number = 0;
        
        // Ad break timing information
        std::chrono::milliseconds ad_break_duration{0};  // Duration from SCTE-35 OUT marker
        
        // Timing information for continuity correction
        std::chrono::steady_clock::time_point expected_start_time;
        std::chrono::steady_clock::time_point actual_start_time;
    };

    // Transport Stream packet with continuity information
    struct TSPacketInfo {
        uint8_t data[188];  // Standard TS packet size
        uint16_t pid = 0;
        uint8_t continuity_counter = 0;
        bool payload_unit_start = false;
        bool discontinuity_indicator = false;
        bool adaptation_field_present = false;
        std::chrono::steady_clock::time_point timestamp;
        
        // Continuity correction fields
        uint8_t original_continuity_counter = 0;
        uint8_t corrected_continuity_counter = 0;
        bool continuity_corrected = false;
        
        TSPacketInfo();
        void ParseHeader();
        bool IsValid() const { return data[0] == 0x47; }
        void SetContinuityCounter(uint8_t counter);
    };

    // Continuity Counter Manager - Tracks and corrects continuity counters across discontinuities
    class ContinuityCounterManager {
    public:
        ContinuityCounterManager();
        
        // Process a TS packet and correct its continuity counter if needed
        bool ProcessPacket(TSPacketInfo& packet);
        
        // Handle discontinuity marker - reset continuity tracking for smooth transition
        void HandleDiscontinuity();
        
        // Reset all PID continuity counters
        void Reset();
        
        // Get statistics
        struct Stats {
            uint64_t packets_processed = 0;
            uint64_t discontinuities_handled = 0;
            uint64_t continuity_corrections_made = 0;
            std::map<uint16_t, uint64_t> pid_packet_counts;
        };
        Stats GetStats() const;
        
    private:
        // Track continuity counter for each PID
        std::map<uint16_t, uint8_t> pid_continuity_counters_;
        
        // Track which PIDs have been seen after a discontinuity
        std::map<uint16_t, bool> pid_seen_after_discontinuity_;
        
        // Statistics
        mutable std::mutex stats_mutex_;
        Stats stats_;
        
        // Helper methods
        bool ShouldHaveContinuityCounter(uint16_t pid) const;
        uint8_t GetNextContinuityCounter(uint16_t pid);
    };

    // HLS Discontinuity Handler - Main class for converting discontinuous HLS to continuous stream
    class HLSDiscontinuityHandler {
    public:
        HLSDiscontinuityHandler();
        ~HLSDiscontinuityHandler();
        
        // Configuration for discontinuity handling
        struct Config {
            bool enable_continuity_correction = true;      // Enable continuity counter correction
            bool enable_timestamp_smoothing = true;        // Enable timestamp smoothing across discontinuities
            bool enable_ad_detection = false;              // Enable SCTE-35 ad marker detection (secondary to discontinuity markers)
            bool preserve_stream_timing = false;           // Preserve original timing vs smooth output
            std::chrono::milliseconds max_gap_tolerance{5000}; // Max gap to bridge smoothly
            
            // Buffer settings for smooth output
            size_t output_buffer_packets = 1000;           // Number of packets to buffer for smooth output
            std::chrono::milliseconds min_buffer_time{100}; // Minimum buffer time before starting output
        };
        
        // Process HLS playlist and convert to continuous stream
        bool ProcessHLSPlaylist(const std::string& playlist_content, 
                               const std::wstring& base_url,
                               std::vector<SegmentInfo>& segments);
        
        // Process HLS segment data and convert to continuous TS packets
        std::vector<TSPacketInfo> ProcessHLSSegment(const std::vector<uint8_t>& segment_data,
                                                   const SegmentInfo& segment_info,
                                                   bool is_first_segment = false);
        
        // Set configuration
        void SetConfig(const Config& config) { config_ = config; }
        Config GetConfig() const { return config_; }
        
        // Get processing statistics
        struct ProcessingStats {
            uint64_t segments_processed = 0;
            uint64_t packets_processed = 0;
            uint64_t discontinuities_detected = 0;
            uint64_t discontinuities_smoothed = 0;
            uint64_t ad_breaks_detected = 0;
            uint64_t continuity_corrections_made = 0;
            std::chrono::milliseconds total_gap_time_bridged{0};
            double average_segment_duration_ms = 0.0;
        };
        ProcessingStats GetStats() const;
        
        // Reset handler state (for new stream)
        void Reset();
        
    private:
        Config config_;
        ContinuityCounterManager continuity_manager_;
        
        // Processing state
        uint64_t segments_processed_ = 0;
        uint64_t packets_processed_ = 0;
        uint64_t discontinuities_detected_ = 0;
        uint64_t discontinuities_smoothed_ = 0;
        uint64_t ad_breaks_detected_ = 0;
        std::chrono::milliseconds total_gap_time_bridged_{0};
        std::chrono::steady_clock::time_point last_segment_end_time_;
        
        // Ad break state tracking
        bool in_ad_break_ = false;
        std::chrono::steady_clock::time_point ad_break_start_time_;
        std::chrono::milliseconds ad_break_expected_duration_{0};
        std::queue<std::vector<TSPacketInfo>> buffered_post_ad_segments_;
        
        // Output smoothing buffer
        std::queue<TSPacketInfo> output_buffer_;
        mutable std::mutex buffer_mutex_;
        
        // Statistics tracking
        mutable std::mutex stats_mutex_;
        std::vector<double> segment_durations_;
        
        // Helper methods
        bool DetectDiscontinuityInSegment(const std::vector<uint8_t>& segment_data);
        void SmoothTimestamps(std::vector<TSPacketInfo>& packets, const SegmentInfo& segment_info);
        void ProcessDiscontinuityMarkers(const SegmentInfo& segment_info);
        std::vector<TSPacketInfo> ExtractTSPackets(const std::vector<uint8_t>& segment_data);
        
        // Ad break handling methods
        void StartAdBreak(const SegmentInfo& segment_info);
        void EndAdBreak();
        bool ShouldBufferSegment(const SegmentInfo& segment_info);
        std::vector<TSPacketInfo> ReleaseBufferedSegments();
        std::chrono::milliseconds ParseAdBreakDuration(const std::string& scte35_line);
        void BufferPacketsForSmoothOutput(const std::vector<TSPacketInfo>& packets);
        std::vector<TSPacketInfo> GetBufferedPacketsForOutput(size_t max_packets = 100);
    };

    // Utility functions
    namespace utils {
        // Parse M3U8 playlist for segments and discontinuity markers
        std::vector<SegmentInfo> ParseM3U8Playlist(const std::string& content, const std::wstring& base_url = L"");
        
        // Check if TS data contains discontinuity indicators
        bool HasDiscontinuityIndicators(const std::vector<uint8_t>& ts_data);
        
        // Extract TS packets from raw data
        std::vector<TSPacketInfo> ExtractTSPacketsFromData(const uint8_t* data, size_t size);
        
        // Convert wide string URL to narrow string for processing
        std::string WideToUtf8(const std::wstring& wide);
        std::wstring Utf8ToWide(const std::string& narrow);
    }

} // namespace hls_discontinuity