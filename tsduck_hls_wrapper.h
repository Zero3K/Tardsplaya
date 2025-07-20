#pragma once
// TSDuck-inspired HLS wrapper for enhanced playlist parsing and timing
// Lightweight implementation focusing on performance improvements for Tardsplaya

#include <string>
#include <vector>
#include <chrono>
#include <map>

namespace tsduck_hls {
    
    // HLS segment with enhanced timing and metadata
    struct MediaSegment {
        std::wstring url;
        std::chrono::milliseconds duration{0};
        std::chrono::milliseconds precise_duration{0}; // More precise timing calculation
        double sequence_number = 0;
        bool has_scte35_out = false;
        bool has_scte35_in = false;
        bool has_discontinuity = false;
        
        // Enhanced ad detection and skipping
        bool is_ad_segment = false;      // True if this segment contains ads
        bool is_ad_break_start = false;  // True if this is the first segment of an ad break
        bool is_ad_break_end = false;    // True if this is the last segment of an ad break
        bool should_skip = false;        // True if this segment should be skipped during playback
        
        // TSDuck-inspired timing fields
        std::chrono::steady_clock::time_point expected_start_time;
        double target_duration = 0.0;
        int64_t program_date_time = 0; // Unix timestamp in ms
        
        MediaSegment() = default;
        MediaSegment(const std::wstring& segment_url, std::chrono::milliseconds dur = std::chrono::milliseconds(0))
            : url(segment_url), duration(dur), precise_duration(dur) {}
            
        // Determine if this segment is part of an ad break
        bool IsAdSegment() const { 
            return is_ad_segment || has_scte35_out || (has_scte35_out && !has_scte35_in);
        }
        
        // Check if this segment should be skipped for ad-free playback
        bool ShouldSkipForAdFree() const {
            return should_skip || IsAdSegment();
        }
    };
    
    // Enhanced playlist with TSDuck-inspired parsing
    class PlaylistParser {
    public:
        // Configuration for ad skipping behavior
        struct AdSkippingConfig {
            bool enable_ad_skipping = false;      // Master toggle for ad skipping
            bool skip_scte35_segments = true;     // Skip segments with SCTE-35 markers
            bool skip_pattern_detected_ads = true; // Skip segments detected by pattern analysis
            bool maintain_stream_continuity = true; // Ensure smooth playback when skipping
            std::chrono::milliseconds max_ad_break_duration{300000}; // Max 5-minute ad breaks
        };
        
        PlaylistParser();
        
        // Parse M3U8 playlist with enhanced timing analysis and ad detection
        bool ParsePlaylist(const std::string& m3u8_content, const AdSkippingConfig& config = AdSkippingConfig{});
        
        // Get segments with ad skipping applied (if enabled)
        std::vector<MediaSegment> GetSegments() const { return segments_; }
        
        // Get segments with ad filtering options
        std::vector<MediaSegment> GetFilteredSegments(bool exclude_ads = false) const;
        
        // Get target duration with more precision
        std::chrono::milliseconds GetTargetDuration() const { return target_duration_; }
        
        // Check if playlist indicates live stream
        bool IsLiveStream() const { return is_live_; }
        
        // Get media sequence number
        int64_t GetMediaSequence() const { return media_sequence_; }
        
        // Enhanced ad detection using TSDuck-style SCTE-35 analysis
        bool HasAdSegments() const;
        int GetAdSegmentCount() const;
        std::chrono::milliseconds GetTotalAdDuration() const;
        
        // Calculate optimal buffer size based on segment analysis
        int GetOptimalBufferSegments() const;
        
        // Get timing information for better buffering
        std::chrono::milliseconds GetPlaylistDuration() const;
        
        // Check for discontinuities that require buffer flushing
        bool HasDiscontinuities() const { return has_discontinuities_; }
        
        // Ad skipping configuration
        void SetAdSkippingConfig(const AdSkippingConfig& config) { ad_config_ = config; }
        const AdSkippingConfig& GetAdSkippingConfig() const { return ad_config_; }
        
    private:
        std::vector<MediaSegment> segments_;
        std::chrono::milliseconds target_duration_{0};
        bool is_live_ = false;
        int64_t media_sequence_ = 0;
        bool has_discontinuities_ = false;
        AdSkippingConfig ad_config_;
        
        // TSDuck-inspired parsing methods
        void ParseSegmentLine(const std::string& line, MediaSegment& current_segment);
        void ParseInfoLine(const std::string& line, MediaSegment& current_segment);
        void ParseDateRangeLine(const std::string& line, MediaSegment& current_segment);
        void ParseScte35Line(const std::string& line, MediaSegment& current_segment);
        
        // Enhanced timing calculations
        void CalculatePreciseTiming();
        
        // Ad detection and skipping methods
        void AnalyzeAdSegments();
        void ApplyAdSkippingLogic();
        bool DetectAdPatternsInSegment(const MediaSegment& segment) const;
        void MarkAdBreakBoundaries();
        
        // Utility methods
        double ExtractFloatFromTag(const std::string& line, const std::string& tag);

    };
    
    // Enhanced buffering calculator using TSDuck timing principles
    class BufferingOptimizer {
    public:
        static int CalculateOptimalBufferSize(const std::vector<MediaSegment>& segments);
        static std::chrono::milliseconds CalculatePreloadTime(const std::vector<MediaSegment>& segments);
        static bool ShouldFlushBuffer(const MediaSegment& current, const MediaSegment& next);
    };
}