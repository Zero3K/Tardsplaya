#include "tsreadex_wrapper.h"
#include "tsreadex/servicefilter.hpp"
#include "tsreadex/util.hpp"
#include <algorithm>

namespace tardsplaya {

TSReadEXProcessor::TSReadEXProcessor() 
    : enabled_(false), pid_filter_table_(8192, false) {
}

TSReadEXProcessor::~TSReadEXProcessor() = default;

bool TSReadEXProcessor::Initialize(const Config& config) {
    config_ = config;
    enabled_ = config.enabled;
    
    if (!enabled_) {
        return true; // Success, but not enabled
    }
    
    try {
        // Create TSReadEX service filter
        filter_ = std::make_unique<CServiceFilter>();
        
        // Configure service selection
        filter_->SetProgramNumberOrIndex(config.program_number);
        
        // Configure audio options
        int audio1_mode = 0;
        int audio2_mode = 0;
        
        if (config.complement_missing_audio) {
            audio1_mode |= 1; // Add silent audio if missing
            audio2_mode |= 1;
        }
        
        if (config.ensure_stereo) {
            audio1_mode |= 4; // Convert mono to stereo
            audio2_mode |= 4;
        }
        
        filter_->SetAudio1Mode(audio1_mode);
        filter_->SetAudio2Mode(audio2_mode);
        
        // Configure caption/superimpose (typically not used for Twitch)
        filter_->SetCaptionMode(2); // Remove captions (not applicable to Twitch)
        filter_->SetSuperimposeMode(2); // Remove superimpose (not applicable to Twitch)
        
        // Setup PID filtering
        SetupPIDFiltering();
        
        // Reset statistics
        stats_ = Stats{};
        
        return true;
    } catch (const std::exception&) {
        enabled_ = false;
        filter_.reset();
        return false;
    }
}

void TSReadEXProcessor::SetupPIDFiltering() {
    // Reset filter table
    std::fill(pid_filter_table_.begin(), pid_filter_table_.end(), false);
    
    // Add excluded PIDs to filter table
    for (int pid : config_.exclude_pids) {
        if (pid >= 0 && pid < 8192) {
            pid_filter_table_[pid] = true;
        }
    }
    
    // Standard PIDs to exclude for stream cleaning
    if (config_.remove_eit) {
        // EIT (Event Information Table) PIDs - program guide data
        pid_filter_table_[0x12] = true; // EIT actual/other
        pid_filter_table_[0x26] = true; // EIT schedule actual/other
        pid_filter_table_[0x27] = true; // EIT schedule actual/other
        
        // Other non-essential PIDs for streaming
        pid_filter_table_[0x14] = true; // TDT/TOT (Time and Date Table)
        pid_filter_table_[0x70] = true; // DIT (Discontinuity Information Table)
        pid_filter_table_[0x71] = true; // SIT (Selection Information Table)
    }
}

bool TSReadEXProcessor::IsPacketFiltered(const uint8_t* packet) const {
    if (!packet || packet[0] != 0x47) {
        return true; // Filter invalid packets
    }
    
    // Extract PID from TS header
    int pid = ((packet[1] & 0x1F) << 8) | packet[2];
    
    return pid_filter_table_[pid];
}

bool TSReadEXProcessor::ProcessChunk(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    output.clear();
    
    if (!IsEnabled()) {
        // Pass through without processing
        output = input;
        return true;
    }
    
    stats_.bytes_input += input.size();
    
    try {
        // Clear previous output from filter
        filter_->ClearPackets();
        
        // Process input in 188-byte TS packet chunks
        const size_t packet_size = 188;
        size_t pos = 0;
        
        while (pos + packet_size <= input.size()) {
            const uint8_t* packet = input.data() + pos;
            
            // Verify sync byte
            if (packet[0] != 0x47) {
                // Try to find next sync byte
                while (pos < input.size() && input[pos] != 0x47) {
                    pos++;
                }
                continue;
            }
            
            stats_.packets_processed++;
            
            // Check if packet should be filtered out
            if (IsPacketFiltered(packet)) {
                stats_.packets_filtered++;
                pos += packet_size;
                continue;
            }
            
            // Process packet through TSReadEX service filter
            filter_->AddPacket(packet);
            
            pos += packet_size;
        }
        
        // Get processed packets from filter
        const auto& filtered_packets = filter_->GetPackets();
        output.insert(output.end(), filtered_packets.begin(), filtered_packets.end());
        
        stats_.bytes_output += output.size();
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void TSReadEXProcessor::Reset() {
    if (filter_) {
        filter_->ClearPackets();
    }
    // Note: Don't reset stats, they accumulate across stream lifetime
}

} // namespace tardsplaya