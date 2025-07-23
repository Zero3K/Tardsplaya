#include "tsduck_hls_wrapper.h"
#include <sstream>
#include <regex>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace tsduck_hls {

PlaylistParser::PlaylistParser() {
    // Initialize with defaults
}

bool PlaylistParser::ParsePlaylist(const std::string& m3u8_content) {
    segments_.clear();
    has_discontinuities_ = false;
    ads_detected_ = false;
    detection_reliable_ = true;
    detection_reason_.clear();
    content_stream_group_ = -1;
    
    std::istringstream stream(m3u8_content);
    std::string line;
    MediaSegment current_segment;
    bool expecting_segment_url = false;
    bool next_segment_follows_discontinuity = false;
    
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
                next_segment_follows_discontinuity = true;
            }
            else if (line.find("#EXT-X-SCTE35-OUT") == 0) {
                current_segment.has_scte35_out = true;
            }
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                current_segment.has_scte35_in = true;
            }
            else if (line.find("#EXT-X-DATERANGE") == 0) {
                ParseDateRangeLine(line, current_segment);
            }
        }
        else if (expecting_segment_url) {
            // This is a segment URL
            current_segment.url = std::wstring(line.begin(), line.end());
            current_segment.sequence_number = media_sequence_ + segments_.size();
            current_segment.follows_discontinuity = next_segment_follows_discontinuity;
            
            segments_.push_back(current_segment);
            
            // Reset for next segment
            current_segment = MediaSegment();
            expecting_segment_url = false;
            next_segment_follows_discontinuity = false;
        }
    }
    
    // Post-processing: calculate precise timing
    CalculatePreciseTiming();
    
    // Perform ad detection if discontinuities were found
    if (has_discontinuities_) {
        // Use conservative mode for initial implementation to ensure stability
        PerformAdDetection(true);
    }
    
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
    }
    catch (const std::exception&) {
        // Default to target duration if parsing fails
        current_segment.duration = target_duration_;
        current_segment.precise_duration = target_duration_;
    }
}

void PlaylistParser::ParseDateRangeLine(const std::string& line, MediaSegment& current_segment) {
    // Basic DATERANGE parsing - no processing needed after ad detection removal
    // DATERANGE tags are validated but not stored per segment
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
    
    for (const auto& segment : segments) {
        avg_duration += segment.target_duration;
        // Note: Ad detection removed - processing all segments equally
    }
    
    if (!segments.empty()) {
        avg_duration /= segments.size();
    }
    
    // Calculate optimal buffer: aim for 10-15 seconds of content for better stability
    int optimal_segments = static_cast<int>(std::ceil(12.0 / avg_duration)); // 12 seconds target (increased from 8)
    
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
    return next.has_discontinuity;
}

// Ad detection implementation (inspired by m3u8adskipper algorithm)
// Enhanced with multi-stream awareness and conservative fallbacks
void PlaylistParser::PerformAdDetection(bool conservative_mode) {
    if (!has_discontinuities_ || segments_.empty()) {
        detection_reason_ = L"No discontinuities found - no ads to detect";
        return; // No ads to detect
    }
    
    // Step 1: Classify segments by discontinuity patterns
    ClassifySegmentsByDiscontinuity();
    
    // Count segments in each group for detailed logging
    int group0_count = 0, group1_count = 0;
    int discontinuity_count = 0;
    for (const auto& segment : segments_) {
        if (segment.stream_group == 0) group0_count++;
        else if (segment.stream_group == 1) group1_count++;
        if (segment.has_discontinuity) discontinuity_count++;
    }
    
    // Step 2: Determine which group contains content (larger group wins)
    content_stream_group_ = DetermineContentGroup();
    
    // Step 3: Validate detection result before applying
    bool is_valid = ValidateAdDetectionResult();
    
    // Add detailed logging for debugging ad detection failures
    detection_reason_ = L"Discontinuities: " + std::to_wstring(discontinuity_count) + 
                       L", Group0: " + std::to_wstring(group0_count) + 
                       L", Group1: " + std::to_wstring(group1_count) + 
                       L", Content group: " + std::to_wstring(content_stream_group_) + 
                       L", Valid: " + (is_valid ? L"yes" : L"no") + 
                       L", Conservative: " + (conservative_mode ? L"yes" : L"no");
    
    // If validation failed, do not apply ad detection regardless of mode
    if (!is_valid) {
        ads_detected_ = false;
        detection_reliable_ = false;
        detection_reason_ += L" - Validation failed: insufficient segment distribution for reliable detection";
        
        // Clear ad markings - treat all segments as content
        for (auto& segment : segments_) {
            segment.is_ad_segment = false;
        }
        return;
    }
    
    // If in conservative mode, apply stricter ratio requirements
    if (conservative_mode) {
        double ratio = (group0_count > group1_count) ? 
            (double)group0_count / (group1_count + 1) : 
            (double)group1_count / (group0_count + 1);
            
        if (ratio < 3.0) {
            // In conservative mode, require 3:1 ratio for ad detection
            ads_detected_ = false;
            detection_reliable_ = false;
            detection_reason_ += L" - Conservative mode: insufficient confidence (ratio " + 
                               std::to_wstring(ratio) + L" < 3.0)";
            
            // Clear ad markings - treat all segments as content
            for (auto& segment : segments_) {
                segment.is_ad_segment = false;
            }
            return;
        }
    }
    
    // Step 4: Apply ad detection result
    for (auto& segment : segments_) {
        segment.is_ad_segment = (segment.stream_group != content_stream_group_);
    }
    
    ads_detected_ = (content_stream_group_ != -1);
    detection_reason_ += ads_detected_ ? 
        L" - Ads detected with confidence" : 
        L" - No clear ad pattern found";
}

void PlaylistParser::ClassifySegmentsByDiscontinuity() {
    if (segments_.empty()) return;
    
    // Following m3u8adskipper logic: separate segments into two groups based on discontinuities
    int current_stream = 0;  // Start with stream 0
    
    for (auto& segment : segments_) {
        // If this segment follows a discontinuity, switch streams BEFORE assigning
        if (segment.follows_discontinuity) {
            current_stream = (current_stream == 0) ? 1 : 0;
        }
        
        // Assign current stream group to this segment
        segment.stream_group = current_stream;
    }
}

int PlaylistParser::DetermineContentGroup() const {
    if (segments_.empty()) return -1;
    
    // Count segments in each group
    int group0_count = 0;
    int group1_count = 0;
    
    for (const auto& segment : segments_) {
        if (segment.stream_group == 0) {
            group0_count++;
        } else if (segment.stream_group == 1) {
            group1_count++;
        }
    }
    
    // Following m3u8adskipper logic: the larger group is assumed to be content
    // (ads are typically shorter than the actual content)
    if (group0_count > group1_count) {
        return 0;
    } else if (group1_count > group0_count) {
        return 1;
    }
    
    // If equal, default to group 0 (no clear ad pattern)
    return 0;
}

bool PlaylistParser::ValidateAdDetectionResult() const {
    if (segments_.empty() || content_stream_group_ == -1) {
        return false;
    }
    
    // Count segments in each group
    int group0_count = 0, group1_count = 0;
    int discontinuity_count = 0;
    
    for (const auto& segment : segments_) {
        if (segment.stream_group == 0) group0_count++;
        else if (segment.stream_group == 1) group1_count++;
        if (segment.has_discontinuity) discontinuity_count++;
    }
    
    // Validation checks:
    
    // 1. Must have at least 2 segments in each group for valid ad detection
    if (group0_count < 2 || group1_count < 2) {
        return false; // Not enough segments in each group
    }
    
    // 2. Must have reasonable number of discontinuities (not too many or too few)
    if (discontinuity_count < 1 || discontinuity_count > segments_.size() / 2) {
        return false; // Too many or too few discontinuities
    }
    
    // 3. The content group should be significantly larger than the ad group
    int content_count = (content_stream_group_ == 0) ? group0_count : group1_count;
    int ad_count = (content_stream_group_ == 0) ? group1_count : group0_count;
    
    if (content_count <= ad_count) {
        return false; // Content should be larger than ads
    }
    
    return true;
}

bool PlaylistParser::DetectAds(bool conservative_mode) {
    PerformAdDetection(conservative_mode);
    return ads_detected_ && detection_reliable_;
}

std::vector<MediaSegment> PlaylistParser::GetContentSegments() const {
    std::vector<MediaSegment> content_segments;
    
    for (const auto& segment : segments_) {
        if (!segment.is_ad_segment) {
            content_segments.push_back(segment);
        }
    }
    
    return content_segments;
}

PlaylistParser::AdDetectionStats PlaylistParser::GetAdDetectionStats() const {
    AdDetectionStats stats;
    stats.total_segments = static_cast<int>(segments_.size());
    stats.discontinuity_count = 0;
    stats.content_segments = 0;
    stats.ad_segments = 0;
    stats.ads_detected = ads_detected_;
    stats.detection_reliable = detection_reliable_;
    stats.detection_reason = detection_reason_;
    
    for (const auto& segment : segments_) {
        if (segment.has_discontinuity) {
            stats.discontinuity_count++;
        }
        if (segment.is_ad_segment) {
            stats.ad_segments++;
        } else {
            stats.content_segments++;
        }
    }
    
    return stats;
}

} // namespace tsduck_hls