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
        
        // TSDuck-inspired timing fields
        std::chrono::steady_clock::time_point expected_start_time;
        double target_duration = 0.0;
        int64_t program_date_time = 0; // Unix timestamp in ms
        
        MediaSegment() = default;
        MediaSegment(const std::wstring& segment_url, std::chrono::milliseconds dur = std::chrono::milliseconds(0))
            : url(segment_url), duration(dur), precise_duration(dur) {}
    };
    
    // Enhanced playlist with TSDuck-inspired parsing
    class PlaylistParser {
    public:
        PlaylistParser();
        
        // Parse M3U8 playlist with enhanced timing analysis
        bool ParsePlaylist(const std::string& m3u8_content);
        
        // Get segments with better timing calculations
        std::vector<MediaSegment> GetSegments() const { return segments_; }
        
        // Get target duration with more precision
        std::chrono::milliseconds GetTargetDuration() const { return target_duration_; }
        
        // Check if playlist indicates live stream
        bool IsLiveStream() const { return is_live_; }
        
        // Get media sequence number
        int64_t GetMediaSequence() const { return media_sequence_; }
        
        // Enhanced ad detection using TSDuck-style SCTE-35 analysis

        
        // Calculate optimal buffer size based on segment analysis
        int GetOptimalBufferSegments() const;
        
        // Get timing information for better buffering
        std::chrono::milliseconds GetPlaylistDuration() const;
        
        // Check for discontinuities that require buffer flushing
        bool HasDiscontinuities() const { return has_discontinuities_; }
        
    private:
        std::vector<MediaSegment> segments_;
        std::chrono::milliseconds target_duration_{0};
        bool is_live_ = false;
        int64_t media_sequence_ = 0;

        bool has_discontinuities_ = false;
        
        // TSDuck-inspired parsing methods
        void ParseSegmentLine(const std::string& line, MediaSegment& current_segment);
        void ParseInfoLine(const std::string& line, MediaSegment& current_segment);
        void ParseDateRangeLine(const std::string& line, MediaSegment& current_segment);
        void ParseScte35Line(const std::string& line, MediaSegment& current_segment);
        
        // Enhanced timing calculations
        void CalculatePreciseTiming();

        
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