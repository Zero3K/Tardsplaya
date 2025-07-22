#include "hls_discontinuity_handler.h"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <regex>

namespace hls_discontinuity {

// TSPacketInfo implementation
TSPacketInfo::TSPacketInfo() {
    memset(data, 0, sizeof(data));
    timestamp = std::chrono::steady_clock::now();
}

void TSPacketInfo::ParseHeader() {
    if (!IsValid()) return;
    
    // Extract PID (13 bits)
    pid = ((data[1] & 0x1F) << 8) | data[2];
    
    // Extract payload unit start indicator
    payload_unit_start = (data[1] & 0x40) != 0;
    
    // Extract adaptation field control and continuity counter
    adaptation_field_present = (data[3] & 0x20) != 0;
    original_continuity_counter = data[3] & 0x0F;
    continuity_counter = original_continuity_counter;
    
    // Extract discontinuity indicator from adaptation field if present
    if (adaptation_field_present && data[4] > 0 && data[4] < 184) {
        discontinuity_indicator = (data[5] & 0x80) != 0;
    }
}

void TSPacketInfo::SetContinuityCounter(uint8_t counter) {
    corrected_continuity_counter = counter;
    continuity_corrected = (counter != original_continuity_counter);
    
    // Update the actual packet data
    data[3] = (data[3] & 0xF0) | (counter & 0x0F);
    continuity_counter = counter;
}

// ContinuityCounterManager implementation
ContinuityCounterManager::ContinuityCounterManager() {
}

bool ContinuityCounterManager::ProcessPacket(TSPacketInfo& packet) {
    packet.ParseHeader();
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.packets_processed++;
    stats_.pid_packet_counts[packet.pid]++;
    
    // Skip packets that don't need continuity counter tracking
    if (!ShouldHaveContinuityCounter(packet.pid)) {
        return true;
    }
    
    // Handle packets after discontinuity differently
    auto it = pid_seen_after_discontinuity_.find(packet.pid);
    if (it != pid_seen_after_discontinuity_.end() && !it->second) {
        // First packet for this PID after discontinuity - reset tracking
        pid_continuity_counters_[packet.pid] = packet.original_continuity_counter;
        pid_seen_after_discontinuity_[packet.pid] = true;
        return true;
    }
    
    // Check if we need to correct continuity counter
    auto counter_it = pid_continuity_counters_.find(packet.pid);
    if (counter_it != pid_continuity_counters_.end()) {
        uint8_t expected_counter = (counter_it->second + 1) & 0x0F;
        
        if (packet.original_continuity_counter != expected_counter) {
            // Discontinuity in continuity counter - correct it
            packet.SetContinuityCounter(expected_counter);
            stats_.continuity_corrections_made++;
        }
        
        pid_continuity_counters_[packet.pid] = packet.continuity_counter;
    } else {
        // First packet for this PID
        pid_continuity_counters_[packet.pid] = packet.original_continuity_counter;
    }
    
    return true;
}

void ContinuityCounterManager::HandleDiscontinuity() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.discontinuities_handled++;
    
    // Mark all PIDs as not seen after discontinuity
    for (auto& pair : pid_continuity_counters_) {
        pid_seen_after_discontinuity_[pair.first] = false;
    }
}

void ContinuityCounterManager::Reset() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    pid_continuity_counters_.clear();
    pid_seen_after_discontinuity_.clear();
    stats_ = Stats{};
}

ContinuityCounterManager::Stats ContinuityCounterManager::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool ContinuityCounterManager::ShouldHaveContinuityCounter(uint16_t pid) const {
    // PIDs that should have continuity counters (exclude NULL packets and PSI with no payload)
    return pid != 0x1FFF && // Not NULL packet
           (pid >= 0x20 || pid == 0x00 || (pid >= 0x10 && pid <= 0x1F)); // Program streams or PSI with payload
}

uint8_t ContinuityCounterManager::GetNextContinuityCounter(uint16_t pid) {
    auto it = pid_continuity_counters_.find(pid);
    if (it != pid_continuity_counters_.end()) {
        return (it->second + 1) & 0x0F;
    }
    return 0;
}

// HLSDiscontinuityHandler implementation
HLSDiscontinuityHandler::HLSDiscontinuityHandler() {
    last_segment_end_time_ = std::chrono::steady_clock::now();
}

HLSDiscontinuityHandler::~HLSDiscontinuityHandler() {
}

bool HLSDiscontinuityHandler::ProcessHLSPlaylist(const std::string& playlist_content, 
                                                 const std::wstring& base_url,
                                                 std::vector<SegmentInfo>& segments) {
    segments.clear();
    
    std::istringstream stream(playlist_content);
    std::string line;
    SegmentInfo current_segment;
    bool expecting_segment_url = false;
    uint64_t sequence_number = 0;
    
    while (std::getline(stream, line)) {
        // Remove trailing whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // Parse tag lines
            if (line.find("#EXTINF:") == 0) {
                // Parse duration
                size_t colon_pos = line.find(':');
                if (colon_pos != std::string::npos) {
                    size_t comma_pos = line.find(',', colon_pos);
                    std::string duration_str;
                    
                    if (comma_pos != std::string::npos) {
                        duration_str = line.substr(colon_pos + 1, comma_pos - colon_pos - 1);
                    } else {
                        duration_str = line.substr(colon_pos + 1);
                    }
                    
                    try {
                        double duration_seconds = std::stod(duration_str);
                        current_segment.duration = std::chrono::milliseconds(
                            static_cast<int64_t>(duration_seconds * 1000));
                    } catch (const std::exception&) {
                        current_segment.duration = std::chrono::milliseconds(6000); // Default 6 seconds
                    }
                }
                expecting_segment_url = true;
            }
            else if (line.find("#EXT-X-DISCONTINUITY") == 0) {
                current_segment.has_discontinuity = true;
                
                std::lock_guard<std::mutex> lock(stats_mutex_);
                discontinuities_detected_++;
            }
            else if (line.find("#EXT-X-SCTE35-OUT") == 0) {
                current_segment.has_scte35_out = true;
                if (config_.enable_ad_detection) {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ad_breaks_detected_++;
                }
            }
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                current_segment.has_scte35_in = true;
            }
        }
        else if (expecting_segment_url) {
            // This is a segment URL
            if (base_url.empty()) {
                current_segment.url = utils::Utf8ToWide(line);
            } else {
                // Resolve relative URL
                if (line.find("http") == 0) {
                    current_segment.url = utils::Utf8ToWide(line);
                } else {
                    std::wstring base_path = base_url;
                    size_t last_slash = base_path.find_last_of(L'/');
                    if (last_slash != std::wstring::npos) {
                        base_path = base_path.substr(0, last_slash + 1);
                    }
                    current_segment.url = base_path + utils::Utf8ToWide(line);
                }
            }
            
            current_segment.sequence_number = sequence_number++;
            segments.push_back(current_segment);
            
            // Track segment durations for statistics
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                segment_durations_.push_back(static_cast<double>(current_segment.duration.count()));
            }
            
            // Reset for next segment
            current_segment = SegmentInfo();
            expecting_segment_url = false;
        }
    }
    
    return !segments.empty();
}

std::vector<TSPacketInfo> HLSDiscontinuityHandler::ProcessHLSSegment(const std::vector<uint8_t>& segment_data,
                                                                     const SegmentInfo& segment_info,
                                                                     bool is_first_segment) {
    std::vector<TSPacketInfo> result;
    
    // Extract TS packets from segment data
    std::vector<TSPacketInfo> raw_packets = ExtractTSPackets(segment_data);
    if (raw_packets.empty()) {
        return result;
    }
    
    // Handle discontinuity markers
    if (segment_info.has_discontinuity) {
        ProcessDiscontinuityMarkers(segment_info);
    }
    
    // Process each packet for continuity correction
    for (auto& packet : raw_packets) {
        if (config_.enable_continuity_correction) {
            continuity_manager_.ProcessPacket(packet);
        }
        
        result.push_back(packet);
        packets_processed_++;
    }
    
    // Apply timestamp smoothing if enabled
    if (config_.enable_timestamp_smoothing) {
        SmoothTimestamps(result, segment_info);
    }
    
    segments_processed_++;
    
    return result;
}

HLSDiscontinuityHandler::ProcessingStats HLSDiscontinuityHandler::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    ProcessingStats stats;
    stats.segments_processed = segments_processed_;
    stats.packets_processed = packets_processed_;
    stats.discontinuities_detected = discontinuities_detected_;
    stats.discontinuities_smoothed = discontinuities_smoothed_;
    stats.ad_breaks_detected = ad_breaks_detected_;
    stats.total_gap_time_bridged = total_gap_time_bridged_;
    
    // Get continuity corrections from continuity manager
    auto continuity_stats = continuity_manager_.GetStats();
    stats.continuity_corrections_made = continuity_stats.continuity_corrections_made;
    
    // Calculate average segment duration
    if (!segment_durations_.empty()) {
        double total = 0.0;
        for (double duration : segment_durations_) {
            total += duration;
        }
        stats.average_segment_duration_ms = total / segment_durations_.size();
    }
    
    return stats;
}

void HLSDiscontinuityHandler::Reset() {
    continuity_manager_.Reset();
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    segments_processed_ = 0;
    packets_processed_ = 0;
    discontinuities_detected_ = 0;
    discontinuities_smoothed_ = 0;
    ad_breaks_detected_ = 0;
    total_gap_time_bridged_ = std::chrono::milliseconds(0);
    segment_durations_.clear();
    last_segment_end_time_ = std::chrono::steady_clock::now();
    
    // Clear output buffer
    std::lock_guard<std::mutex> buffer_lock(buffer_mutex_);
    while (!output_buffer_.empty()) {
        output_buffer_.pop();
    }
}

bool HLSDiscontinuityHandler::DetectDiscontinuityInSegment(const std::vector<uint8_t>& segment_data) {
    return utils::HasDiscontinuityIndicators(segment_data);
}

void HLSDiscontinuityHandler::SmoothTimestamps(std::vector<TSPacketInfo>& packets, const SegmentInfo& segment_info) {
    if (packets.empty()) return;
    
    auto now = std::chrono::steady_clock::now();
    auto segment_duration = segment_info.duration;
    
    // Calculate time per packet for smooth distribution
    auto time_per_packet = segment_duration / packets.size();
    
    for (size_t i = 0; i < packets.size(); ++i) {
        packets[i].timestamp = now + (time_per_packet * i);
    }
}

void HLSDiscontinuityHandler::ProcessDiscontinuityMarkers(const SegmentInfo& segment_info) {
    if (segment_info.has_discontinuity) {
        continuity_manager_.HandleDiscontinuity();
        
        std::lock_guard<std::mutex> lock(stats_mutex_);
        discontinuities_smoothed_++;
    }
}

std::vector<TSPacketInfo> HLSDiscontinuityHandler::ExtractTSPackets(const std::vector<uint8_t>& segment_data) {
    return utils::ExtractTSPacketsFromData(segment_data.data(), segment_data.size());
}

// Utility functions implementation
namespace utils {

std::vector<SegmentInfo> ParseM3U8Playlist(const std::string& content, const std::wstring& base_url) {
    HLSDiscontinuityHandler handler;
    std::vector<SegmentInfo> segments;
    handler.ProcessHLSPlaylist(content, base_url, segments);
    return segments;
}

bool HasDiscontinuityIndicators(const std::vector<uint8_t>& ts_data) {
    // Check for discontinuity indicators in TS packets
    const size_t packet_size = 188;
    
    for (size_t offset = 0; offset + packet_size <= ts_data.size(); offset += packet_size) {
        const uint8_t* packet = &ts_data[offset];
        
        // Check for valid TS packet
        if (packet[0] != 0x47) {
            continue;
        }
        
        // Check for adaptation field and discontinuity indicator
        bool has_adaptation = (packet[3] & 0x20) != 0;
        if (has_adaptation && packet[4] > 0) {
            bool discontinuity = (packet[5] & 0x80) != 0;
            if (discontinuity) {
                return true;
            }
        }
    }
    
    return false;
}

std::vector<TSPacketInfo> ExtractTSPacketsFromData(const uint8_t* data, size_t size) {
    std::vector<TSPacketInfo> packets;
    const size_t packet_size = 188;
    
    for (size_t offset = 0; offset + packet_size <= size; offset += packet_size) {
        const uint8_t* packet_data = data + offset;
        
        // Check for valid TS packet sync byte
        if (packet_data[0] != 0x47) {
            continue;
        }
        
        TSPacketInfo packet;
        memcpy(packet.data, packet_data, packet_size);
        packet.ParseHeader();
        
        packets.push_back(packet);
    }
    
    return packets;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 1) return std::string();
    
    std::string result(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size_needed, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& narrow) {
    if (narrow.empty()) return std::wstring();
    
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
    if (size_needed <= 1) return std::wstring();
    
    std::wstring result(size_needed - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, &result[0], size_needed);
    return result;
}

} // namespace utils
} // namespace hls_discontinuity