#include "tsduck_hls_wrapper.h"
#include <sstream>
#include <regex>
#include <algorithm>
#include <cmath>

namespace tsduck_hls {

PlaylistParser::PlaylistParser() {
    // Initialize with defaults
}

bool PlaylistParser::ParsePlaylist(const std::string& m3u8_content) {
    segments_.clear();
    has_ad_markers_ = false;
    has_discontinuities_ = false;
    
    std::istringstream stream(m3u8_content);
    std::string line;
    MediaSegment current_segment;
    bool expecting_segment_url = false;
    
    while (std::getline(stream, line)) {
        // Remove trailing whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // Parse tag lines
            if (line.find("#EXTINF:") == 0) {
                ParseInfoLine(line, current_segment);
                expecting_segment_url = true;
            }
            else if (line.find("#EXT-X-TARGETDURATION:") == 0) {
                double target = ExtractFloatFromTag(line, "#EXT-X-TARGETDURATION:");
                target_duration_ = std::chrono::milliseconds(static_cast<int64_t>(target * 1000));
            }
            else if (line.find("#EXT-X-MEDIA-SEQUENCE:") == 0) {
                media_sequence_ = static_cast<int64_t>(ExtractFloatFromTag(line, "#EXT-X-MEDIA-SEQUENCE:"));
            }
            else if (line.find("#EXT-X-ENDLIST") == 0) {
                is_live_ = false;
            }
            else if (line.find("#EXT-X-PLAYLIST-TYPE:") == 0) {
                if (line.find("VOD") != std::string::npos) {
                    is_live_ = false;
                } else if (line.find("EVENT") != std::string::npos || line.find("LIVE") != std::string::npos) {
                    is_live_ = true;
                }
            }
            else if (line.find("#EXT-X-DISCONTINUITY") == 0) {
                current_segment.has_discontinuity = true;
                has_discontinuities_ = true;
            }
            else if (line.find("#EXT-X-SCTE35-OUT") == 0) {
                current_segment.has_scte35_out = true;
                current_segment.is_ad_segment = true;
                has_ad_markers_ = true;
            }
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                current_segment.has_scte35_in = true;
            }
            else if (line.find("#EXT-X-DATERANGE") == 0) {
                ParseDateRangeLine(line, current_segment);
            }
            // Enhanced ad detection patterns
            else if (ContainsAdMarkers(line)) {
                current_segment.is_ad_segment = true;
                has_ad_markers_ = true;
            }
        }
        else if (expecting_segment_url) {
            // This is a segment URL
            current_segment.url = std::wstring(line.begin(), line.end());
            current_segment.sequence_number = media_sequence_ + segments_.size();
            
            segments_.push_back(current_segment);
            
            // Reset for next segment
            current_segment = MediaSegment();
            expecting_segment_url = false;
        }
    }
    
    // Post-processing: calculate precise timing and analyze patterns
    CalculatePreciseTiming();
    AnalyzeAdPatterns();
    
    return !segments_.empty();
}

void PlaylistParser::ParseInfoLine(const std::string& line, MediaSegment& current_segment) {
    // Parse #EXTINF:duration[,title]
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) return;
    
    size_t comma_pos = line.find(',', colon_pos);
    std::string duration_str;
    
    if (comma_pos != std::string::npos) {
        duration_str = line.substr(colon_pos + 1, comma_pos - colon_pos - 1);
    } else {
        duration_str = line.substr(colon_pos + 1);
    }
    
    try {
        double duration_seconds = std::stod(duration_str);
        current_segment.duration = std::chrono::milliseconds(static_cast<int64_t>(duration_seconds * 1000));
        current_segment.precise_duration = current_segment.duration;
        current_segment.target_duration = duration_seconds;
        
        // Conservative ad detection - only flag based on explicit markers, not duration
        // Duration-based detection was too aggressive and flagged normal segments
    }
    catch (const std::exception&) {
        // Default to target duration if parsing fails
        current_segment.duration = target_duration_;
        current_segment.precise_duration = target_duration_;
    }
}

void PlaylistParser::ParseDateRangeLine(const std::string& line, MediaSegment& current_segment) {
    // Enhanced DATERANGE parsing for ad detection
    if (line.find("stitched-ad") != std::string::npos ||
        line.find("STITCHED-AD") != std::string::npos ||
        line.find("MIDROLL") != std::string::npos ||
        line.find("midroll") != std::string::npos) {
        current_segment.is_ad_segment = true;
        has_ad_markers_ = true;
    }
}

void PlaylistParser::ParseScte35Line(const std::string& line, MediaSegment& current_segment) {
    // SCTE-35 parsing - already handled in main parsing loop
    // This could be extended for more sophisticated SCTE-35 analysis
}

double PlaylistParser::ExtractFloatFromTag(const std::string& line, const std::string& tag) {
    size_t pos = line.find(tag);
    if (pos == std::string::npos) return 0.0;
    
    std::string value_str = line.substr(pos + tag.length());
    
    // Remove any trailing characters after the number
    size_t end_pos = 0;
    while (end_pos < value_str.length() && 
           (std::isdigit(value_str[end_pos]) || value_str[end_pos] == '.')) {
        end_pos++;
    }
    
    if (end_pos > 0) {
        value_str = value_str.substr(0, end_pos);
        try {
            return std::stod(value_str);
        }
        catch (const std::exception&) {
            return 0.0;
        }
    }
    
    return 0.0;
}

bool PlaylistParser::ContainsAdMarkers(const std::string& line) {
    // Conservative ad marker detection - only check for explicit ad/SCTE markers
    const std::vector<std::string> ad_patterns = {
        "EXT-X-CUE-OUT", "EXT-X-CUE-IN",
        "EXT-X-SCTE35-OUT", "EXT-X-SCTE35-IN", 
        "SCTE-35", "scte-35"
    };
    
    for (const auto& pattern : ad_patterns) {
        if (line.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}

void PlaylistParser::CalculatePreciseTiming() {
    // TSDuck-style precise timing calculation
    auto current_time = std::chrono::steady_clock::now();
    
    for (size_t i = 0; i < segments_.size(); ++i) {
        auto& segment = segments_[i];
        
        // Calculate expected start time based on cumulative duration
        std::chrono::milliseconds cumulative_duration{0};
        for (size_t j = 0; j < i; ++j) {
            cumulative_duration += segments_[j].precise_duration;
        }
        
        segment.expected_start_time = current_time + cumulative_duration;
        
        // Refine duration based on target duration and neighboring segments
        if (segment.target_duration > 0) {
            // Use more precise duration calculation
            auto precise_ms = static_cast<int64_t>(segment.target_duration * 1000);
            segment.precise_duration = std::chrono::milliseconds(precise_ms);
        }
    }
}

void PlaylistParser::AnalyzeAdPatterns() {
    // Conservative ad pattern analysis - only detect based on explicit markers
    // Previous logic was too aggressive and flagged normal segments as ads
    
    for (size_t i = 0; i < segments_.size(); ++i) {
        auto& segment = segments_[i];
        
        // Only flag as ad if there are explicit SCTE-35 markers
        // Don't use duration or discontinuity-based detection as it's unreliable
        if (segment.has_scte35_out && !segment.has_scte35_in) {
            segment.is_ad_segment = true;
            has_ad_markers_ = true;
        }
    }
}

int PlaylistParser::GetOptimalBufferSegments() const {
    return BufferingOptimizer::CalculateOptimalBufferSize(segments_);
}

std::chrono::milliseconds PlaylistParser::GetPlaylistDuration() const {
    std::chrono::milliseconds total{0};
    for (const auto& segment : segments_) {
        total += segment.precise_duration;
    }
    return total;
}

// BufferingOptimizer implementation
int BufferingOptimizer::CalculateOptimalBufferSize(const std::vector<MediaSegment>& segments) {
    if (segments.empty()) return 8; // Increased default fallback for better stability
    
    // TSDuck-inspired buffer calculation based on segment characteristics
    double avg_duration = 0.0;
    int ad_segments = 0;
    
    for (const auto& segment : segments) {
        avg_duration += segment.target_duration;
        if (segment.is_ad_segment) ad_segments++;
    }
    
    if (!segments.empty()) {
        avg_duration /= segments.size();
    }
    
    // Calculate optimal buffer: aim for 10-15 seconds of content for better stability
    int optimal_segments = static_cast<int>(std::ceil(12.0 / avg_duration)); // 12 seconds target (increased from 8)
    
    // Adjust for ad density - more ads = larger buffer for smoother skipping
    double ad_ratio = static_cast<double>(ad_segments) / segments.size();
    if (ad_ratio > 0.3) { // More than 30% ads
        optimal_segments = static_cast<int>(optimal_segments * 1.5);
    }
    
    // Clamp to reasonable range with higher minimums for stability
    return std::max(6, std::min(optimal_segments, 15)); // Increased minimum from 2 to 6, max from 8 to 15
}

std::chrono::milliseconds BufferingOptimizer::CalculatePreloadTime(const std::vector<MediaSegment>& segments) {
    if (segments.empty()) return std::chrono::milliseconds(3000);
    
    // Calculate based on first few segments
    std::chrono::milliseconds preload_time{0};
    int segments_to_analyze = std::min(static_cast<int>(segments.size()), 3);
    
    for (int i = 0; i < segments_to_analyze; ++i) {
        preload_time += segments[i].precise_duration;
    }
    
    return preload_time;
}

bool BufferingOptimizer::ShouldFlushBuffer(const MediaSegment& current, const MediaSegment& next) {
    // TSDuck-style discontinuity analysis
    return next.has_discontinuity || 
           (current.is_ad_segment != next.is_ad_segment && !current.is_ad_segment);
}

} // namespace tsduck_hls