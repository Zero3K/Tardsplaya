#pragma once

// HLS PTS Discontinuity Reclock - Standalone implementation 
// Based on https://github.com/jjustman/ffmpeg-hls-pts-discontinuity-reclock
// Provides seamless HLS->MPEG-TS or HLS->RTMP re-streaming with PTS reclocking

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace hls_pts_reclock {

    // Configuration for PTS discontinuity handling
    struct ReclockConfig {
        // Enable/disable discontinuity correction
        bool force_monotonicity = true;
        
        // Threshold for detecting discontinuities (in microseconds)
        int64_t discontinuity_threshold = 1000000; // 1 second
        
        // Delta threshold for timestamp jumps
        double delta_threshold = 10.0;
        
        // Error threshold for timestamp correction
        double error_threshold = 3600.0 * 30; // 30 hours
        
        ReclockConfig() = default;
    };

    // Timestamp information for PTS/DTS tracking
    struct TimestampInfo {
        int64_t pts = -1; // AV_NOPTS_VALUE equivalent
        int64_t dts = -1; // AV_NOPTS_VALUE equivalent
        int64_t duration = 0;
        bool has_discontinuity = false;
        
        TimestampInfo() = default;
        TimestampInfo(int64_t p, int64_t d, int64_t dur = 0) 
            : pts(p), dts(d), duration(dur) {}
    };

    // Stream state for tracking monotonicity
    struct StreamState {
        int64_t next_pts = -1;
        int64_t next_dts = -1;
        int64_t last_timestamp = -1;
        int64_t monotonicity_offset = 0;
        bool initialized = false;
        
        StreamState() = default;
    };

    // Main PTS Discontinuity Reclock processor
    class PTSReclocker {
    public:
        PTSReclocker(const ReclockConfig& config = ReclockConfig());
        ~PTSReclocker();

        // Process a packet's timestamps for discontinuity correction
        bool ProcessPacket(TimestampInfo& packet_info, int stream_index = 0);
        
        // Reset state for new stream
        void Reset();
        
        // Get current configuration
        const ReclockConfig& GetConfig() const { return config_; }
        
        // Update configuration
        void SetConfig(const ReclockConfig& config) { config_ = config; }
        
        // Check if discontinuity was detected in last packet
        bool DiscontinuityDetected() const { return last_discontinuity_detected_; }
        
        // Get statistics
        struct Stats {
            int64_t total_packets_processed = 0;
            int64_t discontinuities_detected = 0;
            int64_t timestamp_corrections = 0;
            int64_t total_offset_applied = 0;
        };
        
        const Stats& GetStats() const { return stats_; }

    private:
        ReclockConfig config_;
        std::vector<StreamState> stream_states_;
        bool last_discontinuity_detected_ = false;
        Stats stats_;
        
        // Internal helper methods
        bool DetectDiscontinuity(const TimestampInfo& packet, StreamState& state);
        void ApplyMonotonicityCorrection(TimestampInfo& packet, StreamState& state);
        int64_t CalculateTimeDelta(int64_t current, int64_t previous);
    };

    // Utility functions for HLS processing
    namespace utils {
        // Convert time base between different units
        int64_t RescaleTime(int64_t timestamp, int64_t from_timebase, int64_t to_timebase);
        
        // Check if timestamp is valid (not AV_NOPTS_VALUE)
        bool IsValidTimestamp(int64_t timestamp);
        
        // Format timestamp for logging
        std::string FormatTimestamp(int64_t timestamp);
        
        // Calculate threshold in appropriate time units
        int64_t CalculateThreshold(double threshold_seconds, int64_t timebase);
    }

    // Command line interface for standalone executable
    class CommandLineInterface {
    public:
        struct Arguments {
            std::string input_url;
            std::string output_url;
            std::string output_format = "mpegts"; // or "flv" for RTMP
            bool verbose = false;
            bool debug = false;
            bool use_stdout = false; // Output to stdout instead of file
            ReclockConfig reclock_config;
        };
        
        static bool ParseArguments(int argc, char* argv[], Arguments& args);
        static void PrintUsage(const char* program_name);
        static void PrintVersion();
    };

} // namespace hls_pts_reclock