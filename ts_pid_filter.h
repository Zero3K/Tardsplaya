#pragma once
// Transport Stream PID Filter - Inspired by tspidfilter
// Comprehensive PID filtering and discontinuity handling for MPEG-TS streams

#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <atomic>
#include <memory>
#include <chrono>
#include <functional>

namespace tsduck_transport {

    // Forward declaration
    struct TSPacket;

    // PID filtering mode
    enum class PIDFilterMode {
        ALLOW_LIST,    // Only allow specified PIDs (whitelist)
        BLOCK_LIST,    // Block specified PIDs (blacklist)
        AUTO_DETECT    // Automatically detect and filter problematic PIDs
    };

    // Discontinuity handling mode
    enum class DiscontinuityMode {
        PASS_THROUGH,  // Pass all packets, including discontinuities
        FILTER_OUT,    // Remove packets with discontinuity flag
        LOG_ONLY,      // Log discontinuities but pass packets through
        SMART_FILTER   // Intelligently filter based on PID type and context
    };

    // PID category for smart filtering
    enum class PIDCategory {
        UNKNOWN,       // Unknown PID type
        PAT,           // Program Association Table (PID 0x0000)
        CAT,           // Conditional Access Table (PID 0x0001)
        PMT,           // Program Map Table
        NIT,           // Network Information Table (PID 0x0010)
        SDT,           // Service Description Table (PID 0x0011)
        EIT,           // Event Information Table (PID 0x0012)
        TDT,           // Time and Date Table (PID 0x0014)
        NULL_PACKET,   // Null packets (PID 0x1FFF)
        VIDEO,         // Video elementary stream
        AUDIO,         // Audio elementary stream
        SUBTITLE,      // Subtitle/teletext stream
        DATA,          // Data stream
        PCR,           // PCR-only packets
        PRIVATE        // Private data
    };

    // PID statistics for monitoring
    struct PIDStats {
        uint16_t pid = 0;
        PIDCategory category = PIDCategory::UNKNOWN;
        uint64_t packet_count = 0;
        uint64_t discontinuity_count = 0;
        uint64_t error_count = 0;
        std::chrono::steady_clock::time_point first_seen;
        std::chrono::steady_clock::time_point last_seen;
        uint8_t last_continuity_counter = 0;
        bool continuity_error = false;
        
        // Rate statistics
        double packets_per_second = 0.0;
        double discontinuity_rate = 0.0; // Discontinuities per second
        
        PIDStats(uint16_t p = 0) : pid(p) {
            first_seen = last_seen = std::chrono::steady_clock::now();
        }
    };

    // Transport Stream PID Filter
    class TSPIDFilter {
    public:
        TSPIDFilter();
        ~TSPIDFilter() = default;

        // Configuration methods
        void SetFilterMode(PIDFilterMode mode) { filter_mode_ = mode; }
        void SetDiscontinuityMode(DiscontinuityMode mode) { discontinuity_mode_ = mode; }
        
        // PID list management
        void AddAllowedPID(uint16_t pid);
        void AddBlockedPID(uint16_t pid);
        void RemoveAllowedPID(uint16_t pid);
        void RemoveBlockedPID(uint16_t pid);
        void ClearAllowedPIDs();
        void ClearBlockedPIDs();
        
        // Preset configurations
        void SetupStandardPSIFilter();     // Filter standard PSI/SI tables
        void SetupVideoOnlyFilter();       // Only allow video streams
        void SetupAudioVideoFilter();      // Allow audio and video streams
        void SetupNullPacketFilter();      // Filter out null packets
        void SetupDiscontinuityFilter();   // Focus on discontinuity filtering
        
        // Main filtering method
        bool ShouldPassPacket(const TSPacket& packet);
        
        // Batch filtering for efficiency
        std::vector<TSPacket> FilterPackets(const std::vector<TSPacket>& packets);
        
        // Statistics and monitoring
        const std::unordered_map<uint16_t, PIDStats>& GetPIDStats() const { return pid_stats_; }
        PIDStats GetPIDStats(uint16_t pid) const;
        size_t GetTotalPacketsProcessed() const { return total_packets_processed_; }
        size_t GetPacketsFiltered() const { return packets_filtered_; }
        size_t GetDiscontinuitiesDetected() const { return discontinuities_detected_; }
        
        // Advanced analysis
        std::vector<uint16_t> GetActivePIDs() const;
        std::vector<uint16_t> GetProblematicPIDs() const; // PIDs with high error rates
        double GetOverallDiscontinuityRate() const;
        
        // Reset statistics
        void ResetStats();
        void ResetPIDStats(uint16_t pid);
        
        // Auto-detection of problematic PIDs
        void EnableAutoDetection(bool enable = true) { auto_detection_enabled_ = enable; }
        void SetAutoDetectionThreshold(double threshold) { auto_detection_threshold_ = threshold; }
        
        // Logging callback
        void SetLogCallback(std::function<void(const std::wstring&)> callback) { log_callback_ = callback; }

    private:
        PIDFilterMode filter_mode_;
        DiscontinuityMode discontinuity_mode_;
        
        std::unordered_set<uint16_t> allowed_pids_;
        std::unordered_set<uint16_t> blocked_pids_;
        std::unordered_set<uint16_t> auto_blocked_pids_; // Automatically detected problematic PIDs
        
        mutable std::unordered_map<uint16_t, PIDStats> pid_stats_;
        std::atomic<size_t> total_packets_processed_{0};
        std::atomic<size_t> packets_filtered_{0};
        std::atomic<size_t> discontinuities_detected_{0};
        
        // Auto-detection settings
        bool auto_detection_enabled_ = true;
        double auto_detection_threshold_ = 0.1; // 10% discontinuity rate triggers auto-block
        
        std::function<void(const std::wstring&)> log_callback_;
        
        // Helper methods
        PIDCategory ClassifyPID(uint16_t pid) const;
        void UpdatePIDStats(const TSPacket& packet);
        bool CheckContinuityCounter(const TSPacket& packet);
        bool ShouldFilterDiscontinuity(const TSPacket& packet) const;
        void LogFilterAction(const TSPacket& packet, const std::wstring& action) const;
        void CheckAutoDetection(uint16_t pid);
        
        // PID classification helpers
        bool IsPSIPID(uint16_t pid) const;
        bool IsVideoPID(uint16_t pid) const;
        bool IsAudioPID(uint16_t pid) const;
        bool IsNullPID(uint16_t pid) const;
    };

    // Transport Stream PID Filter Manager
    // High-level interface for managing multiple filters and configurations
    class TSPIDFilterManager {
    public:
        TSPIDFilterManager();
        
        // Preset filter configurations
        enum class FilterPreset {
            NONE,              // No filtering
            BASIC_CLEANUP,     // Remove null packets and obvious errors
            QUALITY_FOCUSED,   // Aggressive filtering for quality
            MINIMAL_STREAM,    // Keep only essential audio/video
            DISCONTINUITY_ONLY,// Focus only on discontinuity filtering
            CUSTOM             // User-defined configuration
        };
        
        // Apply preset configuration
        void ApplyPreset(FilterPreset preset);
        
        // Custom configuration
        void ConfigureFilter(PIDFilterMode filter_mode, DiscontinuityMode disc_mode);
        void AddCustomPIDFilter(uint16_t pid, bool allow);
        
        // Main processing interface
        std::vector<TSPacket> ProcessPackets(const std::vector<TSPacket>& input_packets);
        
        // Statistics aggregation
        struct FilterStats {
            size_t total_input_packets = 0;
            size_t total_output_packets = 0;
            size_t filtered_packets = 0;
            size_t discontinuities_detected = 0;
            double filter_efficiency = 0.0; // Percentage of packets passed through
            std::chrono::steady_clock::time_point processing_start;
            std::chrono::milliseconds processing_time{0};
        };
        
        FilterStats GetStats() const { return stats_; }
        void ResetStats();
        
        // Access underlying filter for advanced configuration
        TSPIDFilter& GetFilter() { return filter_; }
        const TSPIDFilter& GetFilter() const { return filter_; }

    private:
        TSPIDFilter filter_;
        FilterStats stats_;
        FilterPreset current_preset_ = FilterPreset::NONE;
        
        void UpdateStats(size_t input_count, size_t output_count, std::chrono::milliseconds processing_time);
    };

} // namespace tsduck_transport