#include "ts_pid_filter.h"
#include "tsduck_transport_router.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace tsduck_transport {

// TSPIDFilter implementation
TSPIDFilter::TSPIDFilter() 
    : filter_mode_(PIDFilterMode::AUTO_DETECT)
    , discontinuity_mode_(DiscontinuityMode::SMART_FILTER) {
    
    // Initialize with basic allowed PIDs for minimal functionality
    allowed_pids_.insert(0x0000); // PAT
    allowed_pids_.insert(0x0001); // CAT (if present)
}

void TSPIDFilter::AddAllowedPID(uint16_t pid) {
    allowed_pids_.insert(pid);
    // Remove from blocked list if present
    blocked_pids_.erase(pid);
    auto_blocked_pids_.erase(pid);
}

void TSPIDFilter::AddBlockedPID(uint16_t pid) {
    blocked_pids_.insert(pid);
    // Remove from allowed list if present
    allowed_pids_.erase(pid);
}

void TSPIDFilter::RemoveAllowedPID(uint16_t pid) {
    allowed_pids_.erase(pid);
}

void TSPIDFilter::RemoveBlockedPID(uint16_t pid) {
    blocked_pids_.erase(pid);
    auto_blocked_pids_.erase(pid);
}

void TSPIDFilter::ClearAllowedPIDs() {
    allowed_pids_.clear();
}

void TSPIDFilter::ClearBlockedPIDs() {
    blocked_pids_.clear();
    auto_blocked_pids_.clear();
}

void TSPIDFilter::SetupStandardPSIFilter() {
    filter_mode_ = PIDFilterMode::ALLOW_LIST;
    discontinuity_mode_ = DiscontinuityMode::SMART_FILTER;
    
    ClearAllowedPIDs();
    
    // Standard PSI/SI PIDs
    AddAllowedPID(0x0000); // PAT
    AddAllowedPID(0x0001); // CAT
    AddAllowedPID(0x0010); // NIT
    AddAllowedPID(0x0011); // SDT/BAT
    AddAllowedPID(0x0012); // EIT
    AddAllowedPID(0x0014); // TDT/TOT
    
    // Typical PMT PIDs (0x1000-0x1FFF range)
    for (uint16_t pid = 0x1000; pid <= 0x1020; ++pid) {
        AddAllowedPID(pid);
    }
    
    LogFilterAction(TSPacket(), L"Applied Standard PSI Filter configuration");
}

void TSPIDFilter::SetupVideoOnlyFilter() {
    filter_mode_ = PIDFilterMode::BLOCK_LIST;
    discontinuity_mode_ = DiscontinuityMode::SMART_FILTER;
    
    ClearBlockedPIDs();
    
    // Block audio PIDs (common ranges)
    for (uint16_t pid = 0x1100; pid <= 0x11FF; ++pid) {
        AddBlockedPID(pid); // Common audio PID range
    }
    
    // Block null packets
    AddBlockedPID(0x1FFF);
    
    LogFilterAction(TSPacket(), L"Applied Video Only Filter configuration");
}

void TSPIDFilter::SetupAudioVideoFilter() {
    filter_mode_ = PIDFilterMode::BLOCK_LIST;
    discontinuity_mode_ = DiscontinuityMode::FILTER_OUT;
    
    ClearBlockedPIDs();
    
    // Block only problematic PIDs
    AddBlockedPID(0x1FFF); // Null packets
    
    // Block some PSI tables that aren't essential for playback
    AddBlockedPID(0x0012); // EIT (Event Information)
    AddBlockedPID(0x0014); // TDT (Time and Date)
    
    LogFilterAction(TSPacket(), L"Applied Audio/Video Filter configuration");
}

void TSPIDFilter::SetupNullPacketFilter() {
    filter_mode_ = PIDFilterMode::BLOCK_LIST;
    discontinuity_mode_ = DiscontinuityMode::PASS_THROUGH;
    
    ClearBlockedPIDs();
    AddBlockedPID(0x1FFF); // Null packets only
    
    LogFilterAction(TSPacket(), L"Applied Null Packet Filter configuration");
}

void TSPIDFilter::SetupDiscontinuityFilter() {
    filter_mode_ = PIDFilterMode::AUTO_DETECT;
    discontinuity_mode_ = DiscontinuityMode::SMART_FILTER;
    
    // Enable aggressive auto-detection
    auto_detection_enabled_ = true;
    auto_detection_threshold_ = 0.05; // 5% discontinuity rate
    
    LogFilterAction(TSPacket(), L"Applied Discontinuity Filter configuration");
}

bool TSPIDFilter::ShouldPassPacket(const TSPacket& packet) {
    total_packets_processed_++;
    
    // Update statistics for this PID
    UpdatePIDStats(packet);
    
    uint16_t pid = packet.pid;
    
    // Check discontinuity filtering first
    if (packet.discontinuity && !ShouldFilterDiscontinuity(packet)) {
        packets_filtered_++;
        LogFilterAction(packet, L"Filtered due to discontinuity");
        return false;
    }
    
    // Apply PID-based filtering
    bool should_pass = true;
    
    switch (filter_mode_) {
        case PIDFilterMode::ALLOW_LIST:
            should_pass = allowed_pids_.count(pid) > 0;
            break;
            
        case PIDFilterMode::BLOCK_LIST:
            should_pass = (blocked_pids_.count(pid) == 0) && 
                         (auto_blocked_pids_.count(pid) == 0);
            break;
            
        case PIDFilterMode::AUTO_DETECT:
            should_pass = auto_blocked_pids_.count(pid) == 0;
            CheckAutoDetection(pid);
            break;
    }
    
    if (!should_pass) {
        packets_filtered_++;
        LogFilterAction(packet, L"Filtered by PID rule");
    }
    
    return should_pass;
}

std::vector<TSPacket> TSPIDFilter::FilterPackets(const std::vector<TSPacket>& packets) {
    std::vector<TSPacket> filtered_packets;
    filtered_packets.reserve(packets.size()); // Optimize for common case
    
    for (const auto& packet : packets) {
        if (ShouldPassPacket(packet)) {
            filtered_packets.push_back(packet);
        }
    }
    
    return filtered_packets;
}

PIDStats TSPIDFilter::GetPIDStats(uint16_t pid) const {
    auto it = pid_stats_.find(pid);
    return (it != pid_stats_.end()) ? it->second : PIDStats(pid);
}

std::vector<uint16_t> TSPIDFilter::GetActivePIDs() const {
    std::vector<uint16_t> active_pids;
    
    for (const auto& pair : pid_stats_) {
        if (pair.second.packet_count > 0) {
            active_pids.push_back(pair.first);
        }
    }
    
    std::sort(active_pids.begin(), active_pids.end());
    return active_pids;
}

std::vector<uint16_t> TSPIDFilter::GetProblematicPIDs() const {
    std::vector<uint16_t> problematic_pids;
    
    for (const auto& pair : pid_stats_) {
        const auto& stats = pair.second;
        if (stats.packet_count > 10 && stats.discontinuity_rate > auto_detection_threshold_) {
            problematic_pids.push_back(pair.first);
        }
    }
    
    return problematic_pids;
}

double TSPIDFilter::GetOverallDiscontinuityRate() const {
    if (total_packets_processed_ == 0) return 0.0;
    return static_cast<double>(discontinuities_detected_) / total_packets_processed_;
}

void TSPIDFilter::ResetStats() {
    pid_stats_.clear();
    total_packets_processed_ = 0;
    packets_filtered_ = 0;
    discontinuities_detected_ = 0;
}

void TSPIDFilter::ResetPIDStats(uint16_t pid) {
    pid_stats_.erase(pid);
}

PIDCategory TSPIDFilter::ClassifyPID(uint16_t pid) const {
    switch (pid) {
        case 0x0000: return PIDCategory::PAT;
        case 0x0001: return PIDCategory::CAT;
        case 0x0010: return PIDCategory::NIT;
        case 0x0011: return PIDCategory::SDT;
        case 0x0012: return PIDCategory::EIT;
        case 0x0014: return PIDCategory::TDT;
        case 0x1FFF: return PIDCategory::NULL_PACKET;
    }
    
    // PMT PIDs typically in range 0x1000-0x1FFF
    if (pid >= 0x1000 && pid < 0x1FFF) {
        return PIDCategory::PMT; // Could be PMT or elementary stream
    }
    
    // Video streams often in range 0x100-0x1FF
    if (pid >= 0x0100 && pid <= 0x01FF) {
        return PIDCategory::VIDEO;
    }
    
    // Audio streams often in range 0x200-0x2FF
    if (pid >= 0x0200 && pid <= 0x02FF) {
        return PIDCategory::AUDIO;
    }
    
    return PIDCategory::UNKNOWN;
}

void TSPIDFilter::UpdatePIDStats(const TSPacket& packet) {
    uint16_t pid = packet.pid;
    auto& stats = pid_stats_[pid];
    
    if (stats.packet_count == 0) {
        // First packet for this PID
        stats.pid = pid;
        stats.category = ClassifyPID(pid);
        stats.first_seen = packet.timestamp;
    }
    
    stats.packet_count++;
    stats.last_seen = packet.timestamp;
    
    // Check for discontinuity
    if (packet.discontinuity) {
        stats.discontinuity_count++;
        discontinuities_detected_++;
    }
    
    // Check continuity counter for errors
    if (!CheckContinuityCounter(packet)) {
        stats.error_count++;
        stats.continuity_error = true;
    }
    
    // Update rates
    auto time_span = std::chrono::duration_cast<std::chrono::seconds>(
        stats.last_seen - stats.first_seen).count();
    
    if (time_span > 0) {
        stats.packets_per_second = static_cast<double>(stats.packet_count) / time_span;
        stats.discontinuity_rate = static_cast<double>(stats.discontinuity_count) / time_span;
    }
}

bool TSPIDFilter::CheckContinuityCounter(const TSPacket& packet) {
    uint16_t pid = packet.pid;
    
    // Extract continuity counter from packet (last 4 bits of byte 3)
    uint8_t current_cc = packet.data[3] & 0x0F;
    
    auto& stats = pid_stats_[pid];
    
    if (stats.packet_count == 1) {
        // First packet, store the continuity counter
        stats.last_continuity_counter = current_cc;
        return true;
    }
    
    // Check if this is a duplicate packet (same CC)
    if (current_cc == stats.last_continuity_counter) {
        // Duplicate packet or adaptation field only packet
        return !(packet.data[3] & 0x10); // Error if has payload but same CC
    }
    
    // Check if CC incremented correctly
    uint8_t expected_cc = (stats.last_continuity_counter + 1) & 0x0F;
    bool cc_ok = (current_cc == expected_cc);
    
    stats.last_continuity_counter = current_cc;
    return cc_ok;
}

bool TSPIDFilter::ShouldFilterDiscontinuity(const TSPacket& packet) const {
    switch (discontinuity_mode_) {
        case DiscontinuityMode::PASS_THROUGH:
            return true; // Always pass
            
        case DiscontinuityMode::FILTER_OUT:
            return false; // Always filter
            
        case DiscontinuityMode::LOG_ONLY:
            if (log_callback_) {
                LogFilterAction(packet, L"Discontinuity detected (logged only)");
            }
            return true; // Pass but log
            
        case DiscontinuityMode::SMART_FILTER: {
            PIDCategory category = ClassifyPID(packet.pid);
            
            // Always pass discontinuities in essential streams
            if (category == PIDCategory::PAT || 
                category == PIDCategory::PMT ||
                category == PIDCategory::VIDEO ||
                category == PIDCategory::AUDIO) {
                return true;
            }
            
            // Filter discontinuities in non-essential streams
            return false;
        }
    }
    
    return true; // Default to pass
}

void TSPIDFilter::LogFilterAction(const TSPacket& packet, const std::wstring& action) const {
    if (!log_callback_) return;
    
    std::wstringstream msg;
    msg << L"PID Filter: " << action;
    
    if (packet.pid != 0) { // Skip for configuration messages
        msg << L" - PID: 0x" << std::hex << std::setw(4) << std::setfill(L'0') << packet.pid;
        msg << L" (" << std::dec << packet.pid << L")";
        
        PIDCategory category = ClassifyPID(packet.pid);
        switch (category) {
            case PIDCategory::PAT: msg << L" [PAT]"; break;
            case PIDCategory::PMT: msg << L" [PMT]"; break;
            case PIDCategory::VIDEO: msg << L" [VIDEO]"; break;
            case PIDCategory::AUDIO: msg << L" [AUDIO]"; break;
            case PIDCategory::NULL_PACKET: msg << L" [NULL]"; break;
            default: break;
        }
        
        if (packet.discontinuity) {
            msg << L" [DISCONTINUITY]";
        }
    }
    
    log_callback_(msg.str());
}

void TSPIDFilter::CheckAutoDetection(uint16_t pid) {
    if (!auto_detection_enabled_) return;
    
    auto it = pid_stats_.find(pid);
    if (it == pid_stats_.end()) return;
    
    const auto& stats = it->second;
    
    // Only consider PIDs with sufficient packet count
    if (stats.packet_count < 100) return;
    
    // Check if discontinuity rate exceeds threshold
    if (stats.discontinuity_rate > auto_detection_threshold_) {
        if (auto_blocked_pids_.insert(pid).second) {
            // Newly blocked PID
            LogFilterAction(TSPacket(), 
                L"Auto-detected problematic PID: 0x" + 
                std::to_wstring(pid) + L" (discontinuity rate: " + 
                std::to_wstring(stats.discontinuity_rate * 100) + L"%)");
        }
    }
}

bool TSPIDFilter::IsPSIPID(uint16_t pid) const {
    return (pid <= 0x0014) || (pid >= 0x1000 && pid <= 0x1020);
}

bool TSPIDFilter::IsVideoPID(uint16_t pid) const {
    return ClassifyPID(pid) == PIDCategory::VIDEO;
}

bool TSPIDFilter::IsAudioPID(uint16_t pid) const {
    return ClassifyPID(pid) == PIDCategory::AUDIO;
}

bool TSPIDFilter::IsNullPID(uint16_t pid) const {
    return pid == 0x1FFF;
}

// TSPIDFilterManager implementation
TSPIDFilterManager::TSPIDFilterManager() {
    stats_.processing_start = std::chrono::steady_clock::now();
}

void TSPIDFilterManager::ApplyPreset(FilterPreset preset) {
    current_preset_ = preset;
    
    switch (preset) {
        case FilterPreset::NONE:
            filter_.SetFilterMode(PIDFilterMode::AUTO_DETECT);
            filter_.SetDiscontinuityMode(DiscontinuityMode::PASS_THROUGH);
            filter_.ClearAllowedPIDs();
            filter_.ClearBlockedPIDs();
            break;
            
        case FilterPreset::BASIC_CLEANUP:
            filter_.SetupNullPacketFilter();
            filter_.SetDiscontinuityMode(DiscontinuityMode::LOG_ONLY);
            break;
            
        case FilterPreset::QUALITY_FOCUSED:
            filter_.SetupAudioVideoFilter();
            filter_.EnableAutoDetection(true);
            filter_.SetAutoDetectionThreshold(0.02); // 2% threshold
            break;
            
        case FilterPreset::MINIMAL_STREAM:
            filter_.SetupVideoOnlyFilter();
            filter_.SetDiscontinuityMode(DiscontinuityMode::FILTER_OUT);
            break;
            
        case FilterPreset::DISCONTINUITY_ONLY:
            filter_.SetupDiscontinuityFilter();
            break;
            
        case FilterPreset::CUSTOM:
            // User will configure manually
            break;
    }
}

void TSPIDFilterManager::ConfigureFilter(PIDFilterMode filter_mode, DiscontinuityMode disc_mode) {
    current_preset_ = FilterPreset::CUSTOM;
    filter_.SetFilterMode(filter_mode);
    filter_.SetDiscontinuityMode(disc_mode);
}

void TSPIDFilterManager::AddCustomPIDFilter(uint16_t pid, bool allow) {
    if (allow) {
        filter_.AddAllowedPID(pid);
    } else {
        filter_.AddBlockedPID(pid);
    }
}

std::vector<TSPacket> TSPIDFilterManager::ProcessPackets(const std::vector<TSPacket>& input_packets) {
    auto start_time = std::chrono::steady_clock::now();
    
    auto output_packets = filter_.FilterPackets(input_packets);
    
    auto end_time = std::chrono::steady_clock::now();
    auto processing_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    UpdateStats(input_packets.size(), output_packets.size(), processing_time);
    
    return output_packets;
}

void TSPIDFilterManager::ResetStats() {
    stats_ = FilterStats();
    stats_.processing_start = std::chrono::steady_clock::now();
    filter_.ResetStats();
}

void TSPIDFilterManager::UpdateStats(size_t input_count, size_t output_count, std::chrono::milliseconds processing_time) {
    stats_.total_input_packets += input_count;
    stats_.total_output_packets += output_count;
    stats_.filtered_packets += (input_count - output_count);
    stats_.processing_time += processing_time;
    
    if (stats_.total_input_packets > 0) {
        stats_.filter_efficiency = static_cast<double>(stats_.total_output_packets) / stats_.total_input_packets;
    }
    
    stats_.discontinuities_detected = filter_.GetDiscontinuitiesDetected();
}

} // namespace tsduck_transport