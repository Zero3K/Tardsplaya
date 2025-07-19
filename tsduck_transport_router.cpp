#include "tsduck_transport_router.h"
#include "twitch_api.h"
#include "playlist_parser.h"
#include "tsduck_hls_wrapper.h"
#include "stream_resource_manager.h"
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cstring>

// Forward declarations
extern bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token = nullptr);
std::wstring Utf8ToWide(const std::string& str);
std::string WideToUtf8(const std::wstring& str);
void AddDebugLog(const std::wstring& msg);

// HttpGetBinary implementation for transport stream router with retry logic
bool HttpGetBinary(const std::wstring& url, std::vector<uint8_t>& out, std::atomic<bool>* cancel_token = nullptr) {
    const int max_attempts = 3;
    
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (cancel_token && cancel_token->load()) return false;
        
        URL_COMPONENTS uc = { sizeof(uc) };
        wchar_t host[256] = L"", path[2048] = L"";
        uc.lpszHostName = host; uc.dwHostNameLength = 255;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
            if (attempt < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                continue;
            }
            return false;
        }

        HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
        if (!hSession) {
            if (attempt < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                continue;
            }
            return false;
        }
        
        HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        if (!hConnect) { 
            WinHttpCloseHandle(hSession);
            if (attempt < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                continue;
            }
            return false; 
        }
        
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (attempt < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                continue;
            }
            return false;
        }
        
        // For HTTPS, ignore certificate errors for compatibility
        if (uc.nScheme == INTERNET_SCHEME_HTTPS) {
            DWORD dwSecurityFlags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                   SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                   SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                   SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecurityFlags, sizeof(dwSecurityFlags));
        }
        
        BOOL res = WinHttpSendRequest(hRequest, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hRequest, 0);
        if (!res) { 
            WinHttpCloseHandle(hRequest); 
            WinHttpCloseHandle(hConnect); 
            WinHttpCloseHandle(hSession);
            if (attempt < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                continue;
            }
            return false; 
        }

        DWORD dwSize = 0;
        out.clear();
        bool error = false;
        do {
            if (cancel_token && cancel_token->load()) { 
                error = true; 
                break; 
            }
            
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            
            size_t prev_size = out.size();
            out.resize(prev_size + dwSize);
            
            if (!WinHttpReadData(hRequest, out.data() + prev_size, dwSize, &dwDownloaded) || dwDownloaded == 0) {
                error = true;
                break;
            }
            
            if (dwDownloaded < dwSize) {
                out.resize(prev_size + dwDownloaded);
            }
        } while (dwSize > 0);

        WinHttpCloseHandle(hRequest); 
        WinHttpCloseHandle(hConnect); 
        WinHttpCloseHandle(hSession);
        
        if (!error && !out.empty()) return true;
        
        if (attempt < max_attempts - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
    }
    
    return false;
}

// Minimal HTTP implementation for standalone DLL
#ifdef BUILD_DLL
bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token) {
    // Simplified HTTP implementation for DLL version
    // In production, this would use WinHTTP or similar
    out = ""; // Placeholder - would fetch actual content
    return false; // Return false for now to indicate this is a stub
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

void AddDebugLog(const std::wstring& msg) {
    // Placeholder - would output to debug console or file
    // For DLL version, logging is optional
}
#endif

namespace tsduck_transport {

// Helper: join relative URL to base
static std::wstring JoinUrl(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}



// TSPacket implementation
void TSPacket::ParseHeader() {
    if (!IsValid()) return;
    
    // Extract PID (13 bits from bytes 1-2)
    pid = ((data[1] & 0x1F) << 8) | data[2];
    
    // Extract payload unit start indicator
    payload_unit_start = (data[1] & 0x40) != 0;
    
    // Extract discontinuity indicator from adaptation field if present
    bool has_adaptation = (data[3] & 0x20) != 0;
    if (has_adaptation && data[4] > 0) {
        discontinuity = (data[5] & 0x80) != 0;
    }
}

// Frame Number Tagging implementations
void TSPacket::SetFrameInfo(uint64_t global_frame, uint32_t segment_frame, bool key_frame, std::chrono::milliseconds duration) {
    frame_number = global_frame;
    segment_frame_number = segment_frame;
    is_key_frame = key_frame;
    frame_duration = duration;
}

void TSPacket::SetVideoInfo(uint64_t video_frame, bool is_video, bool is_audio) {
    video_frame_number = video_frame;
    is_video_packet = is_video;
    is_audio_packet = is_audio;
    video_sync_lost = false; // Reset sync status when setting video info
}

std::wstring TSPacket::GetFrameDebugInfo() const {
    std::wstring info = L"Frame#" + std::to_wstring(frame_number) + 
                       L" Seg#" + std::to_wstring(segment_frame_number) +
                       L" PID:" + std::to_wstring(pid);
    
    if (is_video_packet) info += L" [VIDEO]";
    if (is_audio_packet) info += L" [AUDIO]";
    if (is_key_frame) info += L" [KEY]";
    if (payload_unit_start) info += L" [START]";
    if (discontinuity) info += L" [DISC]";
    if (video_sync_lost) info += L" [SYNC_LOST]";
    
    if (frame_duration.count() > 0) {
        info += L" (" + std::to_wstring(frame_duration.count()) + L"ms)";
    }
    
    return info;
}

bool TSPacket::IsFrameDropDetected(const TSPacket& previous_packet) const {
    // Detect frame drops by checking sequence continuity
    if (frame_number <= previous_packet.frame_number) {
        return false; // Not a drop, possibly duplicate or reordered
    }
    
    // Check if we skipped frame numbers (indicating dropped frames)
    uint64_t expected_frame = previous_packet.frame_number + 1;
    return frame_number > expected_frame;
}

bool TSPacket::IsVideoSyncValid() const {
    return is_video_packet && !video_sync_lost;
}

// TSBuffer implementation
TSBuffer::TSBuffer(size_t max_packets) : max_packets_(max_packets), producer_active_(true) {
}

bool TSBuffer::AddPacket(const TSPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // In low-latency mode, more aggressively drop old packets
    if (low_latency_mode_ && packet_queue_.size() >= max_packets_ / 2) {
        // Drop multiple old packets to make room for new ones
        size_t packets_to_drop = std::min(packet_queue_.size() / 4, size_t(10));
        for (size_t i = 0; i < packets_to_drop && !packet_queue_.empty(); ++i) {
            packet_queue_.pop();
        }
    } else if (packet_queue_.size() >= max_packets_) {
        // Standard mode - remove oldest packet to make room
        packet_queue_.pop();
    }
    
    packet_queue_.push(packet);
    return true;
}

bool TSBuffer::GetNextPacket(TSPacket& packet, std::chrono::milliseconds timeout) {
    auto start_time = std::chrono::steady_clock::now();
    
    while (std::chrono::steady_clock::now() - start_time < timeout) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!packet_queue_.empty()) {
                packet = packet_queue_.front();
                packet_queue_.pop();
                return true;
            }
        }
        
        if (!producer_active_) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    return false;
}

size_t TSBuffer::GetBufferedPackets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_queue_.size();
}

bool TSBuffer::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_queue_.empty();
}

bool TSBuffer::IsFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_queue_.size() >= max_packets_;
}

void TSBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!packet_queue_.empty()) {
        packet_queue_.pop();
    }
}

void TSBuffer::Reset() {
    Clear();
    producer_active_ = true;
}

void TSBuffer::SignalEndOfStream() {
    producer_active_ = false;
}

// HLSToTSConverter implementation
HLSToTSConverter::HLSToTSConverter() {
    Reset();
}

void HLSToTSConverter::Reset() {
    continuity_counter_ = 0;
    pat_sent_ = false;
    pmt_sent_ = false;
    
    // Frame Number Tagging: Reset frame counters
    global_frame_counter_ = 0;
    segment_frame_counter_ = 0;
    last_frame_time_ = std::chrono::steady_clock::now();
    estimated_frame_duration_ = std::chrono::milliseconds(33); // Reset to default ~30fps
    
    // Reset stream type detection
    detected_video_pid_ = 0;
    detected_audio_pid_ = 0;
}

std::vector<TSPacket> HLSToTSConverter::ConvertSegment(const std::vector<uint8_t>& hls_data, bool is_first_segment) {
    std::vector<TSPacket> ts_packets;
    
    if (hls_data.empty()) {
        return ts_packets;
    }
    
    // HLS segments are already MPEG-TS formatted data
    // Extract and validate existing TS packets with better synchronization
    size_t data_size = hls_data.size();
    const uint8_t* data_ptr = hls_data.data();
    
    // Find first sync byte to ensure proper alignment
    size_t sync_offset = 0;
    for (; sync_offset < data_size; ++sync_offset) {
        if (data_ptr[sync_offset] == 0x47) {
            // Check if this looks like a valid TS packet start
            if (sync_offset + TS_PACKET_SIZE <= data_size) {
                // Look ahead to see if next packet is also aligned
                if (sync_offset + TS_PACKET_SIZE < data_size && 
                    data_ptr[sync_offset + TS_PACKET_SIZE] == 0x47) {
                    break; // Found valid sync
                }
            }
        }
    }
    
    if (sync_offset >= data_size) {
        return ts_packets; // No valid sync found
    }
    
    // Frame Number Tagging: Reset segment frame counter for new segment
    if (is_first_segment) {
        segment_frame_counter_ = 0;
        last_frame_time_ = std::chrono::steady_clock::now();
    }
    
    // Process data in 188-byte TS packet chunks starting from sync position
    for (size_t offset = sync_offset; offset + TS_PACKET_SIZE <= data_size; offset += TS_PACKET_SIZE) {
        TSPacket packet;
        
        // Copy packet data
        memcpy(packet.data, data_ptr + offset, TS_PACKET_SIZE);
        packet.timestamp = std::chrono::steady_clock::now();
        
        // Validate sync byte
        if (packet.data[0] != 0x47) {
            // Sync lost, try to resynchronize
            break;
        }
        
        // Parse packet header
        packet.ParseHeader();
        
        // Detect and classify stream types (video/audio)
        DetectStreamTypes(packet);
        
        // Frame Number Tagging: Assign frame numbers for video packets
        if (packet.is_video_packet || packet.payload_unit_start) {
            // Increment frame counters for video packets or new payload units
            global_frame_counter_++;
            segment_frame_counter_++;
            
            // Estimate frame duration based on timing
            auto now = std::chrono::steady_clock::now();
            auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_time_);
            if (time_since_last.count() > 0 && segment_frame_counter_ > 1) {
                estimated_frame_duration_ = time_since_last;
            }
            last_frame_time_ = now;
            
            // Check for key frame indicators in the payload
            bool is_key_frame = false;
            if (packet.payload_unit_start && packet.is_video_packet && packet.data[4] == 0x00) {
                // Look for MPEG start codes that might indicate I-frames
                if (offset + TS_PACKET_SIZE + 8 < data_size) {
                    const uint8_t* payload = data_ptr + offset + 4;
                    // Enhanced I-frame detection
                    for (int i = 0; i < 32 && i < TS_PACKET_SIZE - 8; i++) {
                        if (payload[i] == 0x00 && payload[i+1] == 0x00 && payload[i+2] == 0x01) {
                            uint8_t frame_type = payload[i+3];
                            // MPEG-2 I-frame detection (enhanced)
                            if ((frame_type & 0x38) == 0x08 || frame_type == 0x00) {
                                is_key_frame = true;
                                break;
                            }
                            // H.264 IDR frame detection
                            if ((frame_type & 0x1F) == 0x05) {
                                is_key_frame = true;
                                break;
                            }
                        }
                    }
                }
            }
            
            // Set frame information
            packet.SetFrameInfo(global_frame_counter_, segment_frame_counter_, is_key_frame, estimated_frame_duration_);
            
            // Set video-specific information for video packets
            if (packet.is_video_packet) {
                packet.SetVideoInfo(global_frame_counter_, true, false);
            } else if (packet.is_audio_packet) {
                packet.SetVideoInfo(0, false, true);
            }
        }
        
        // Don't modify continuity counters - preserve original TS packet timing and sequencing
        // The original HLS TS segments should have correct continuity counters
        
        ts_packets.push_back(packet);
    }
    
    return ts_packets;
}

TSPacket HLSToTSConverter::GeneratePAT() {
    TSPacket packet;
    
    // TS Header for PAT (PID 0x0000)
    packet.data[0] = 0x47; // Sync byte
    packet.data[1] = 0x40; // Payload unit start, PID high bits
    packet.data[2] = 0x00; // PID low bits (0x0000)
    packet.data[3] = 0x10 | (continuity_counter_ & 0x0F); // No adaptation, payload present, continuity
    
    // PSI header
    packet.data[4] = 0x00; // Pointer field
    packet.data[5] = 0x00; // Table ID (PAT)
    packet.data[6] = 0xB0; // Section syntax indicator, section length high
    packet.data[7] = 0x0D; // Section length low (13 bytes)
    packet.data[8] = 0x00; packet.data[9] = 0x01; // Transport stream ID
    packet.data[10] = 0xC1; // Version number, current/next indicator
    packet.data[11] = 0x00; // Section number
    packet.data[12] = 0x00; // Last section number
    
    // Program info (one program)
    packet.data[13] = (program_id_ >> 8) & 0xFF; // Program number high
    packet.data[14] = program_id_ & 0xFF; // Program number low
    packet.data[15] = 0xE0 | ((pmt_pid_ >> 8) & 0x1F); // PMT PID high
    packet.data[16] = pmt_pid_ & 0xFF; // PMT PID low
    
    // CRC32 (simplified - would need proper calculation in production)
    uint32_t crc = CalculateCRC32(&packet.data[5], 12);
    packet.data[17] = (crc >> 24) & 0xFF;
    packet.data[18] = (crc >> 16) & 0xFF;
    packet.data[19] = (crc >> 8) & 0xFF;
    packet.data[20] = crc & 0xFF;
    
    // Fill rest with padding
    for (size_t i = 21; i < TS_PACKET_SIZE; ++i) {
        packet.data[i] = 0xFF;
    }
    
    packet.pid = 0x0000;
    packet.payload_unit_start = true;
    continuity_counter_ = (continuity_counter_ + 1) & 0x0F;
    
    return packet;
}

TSPacket HLSToTSConverter::GeneratePMT() {
    TSPacket packet;
    
    // TS Header for PMT
    packet.data[0] = 0x47; // Sync byte
    packet.data[1] = 0x40 | ((pmt_pid_ >> 8) & 0x1F); // Payload unit start, PID high bits
    packet.data[2] = pmt_pid_ & 0xFF; // PID low bits
    packet.data[3] = 0x10 | (continuity_counter_ & 0x0F); // No adaptation, payload present, continuity
    
    // PSI header
    packet.data[4] = 0x00; // Pointer field
    packet.data[5] = 0x02; // Table ID (PMT)
    packet.data[6] = 0xB0; // Section syntax indicator, section length high
    packet.data[7] = 0x17; // Section length low (23 bytes)
    packet.data[8] = (program_id_ >> 8) & 0xFF; // Program number high
    packet.data[9] = program_id_ & 0xFF; // Program number low
    packet.data[10] = 0xC1; // Version number, current/next indicator
    packet.data[11] = 0x00; // Section number
    packet.data[12] = 0x00; // Last section number
    packet.data[13] = 0xE0 | ((video_pid_ >> 8) & 0x1F); // PCR PID high (use video PID)
    packet.data[14] = video_pid_ & 0xFF; // PCR PID low
    packet.data[15] = 0xF0; packet.data[16] = 0x00; // Program info length
    
    // Elementary stream info (video)
    packet.data[17] = 0x1B; // Stream type (H.264 video)
    packet.data[18] = 0xE0 | ((video_pid_ >> 8) & 0x1F); // Elementary PID high
    packet.data[19] = video_pid_ & 0xFF; // Elementary PID low
    packet.data[20] = 0xF0; packet.data[21] = 0x00; // ES info length
    
    // Elementary stream info (audio)
    packet.data[22] = 0x0F; // Stream type (AAC audio)
    packet.data[23] = 0xE0 | ((audio_pid_ >> 8) & 0x1F); // Elementary PID high
    packet.data[24] = audio_pid_ & 0xFF; // Elementary PID low
    packet.data[25] = 0xF0; packet.data[26] = 0x00; // ES info length
    
    // CRC32
    uint32_t crc = CalculateCRC32(&packet.data[5], 22);
    packet.data[27] = (crc >> 24) & 0xFF;
    packet.data[28] = (crc >> 16) & 0xFF;
    packet.data[29] = (crc >> 8) & 0xFF;
    packet.data[30] = crc & 0xFF;
    
    // Fill rest with padding
    for (size_t i = 31; i < TS_PACKET_SIZE; ++i) {
        packet.data[i] = 0xFF;
    }
    
    packet.pid = pmt_pid_;
    packet.payload_unit_start = true;
    continuity_counter_ = (continuity_counter_ + 1) & 0x0F;
    
    return packet;
}

std::vector<TSPacket> HLSToTSConverter::WrapDataInTS(const uint8_t* data, size_t size, uint16_t pid, bool payload_start) {
    std::vector<TSPacket> packets;
    
    size_t remaining = size;
    const uint8_t* current_data = data;
    bool first_packet = payload_start;
    
    while (remaining > 0) {
        TSPacket packet;
        packet.pid = pid;
        packet.payload_unit_start = first_packet;
        
        // TS Header
        packet.data[0] = 0x47; // Sync byte
        packet.data[1] = (first_packet ? 0x40 : 0x00) | ((pid >> 8) & 0x1F); // Payload unit start + PID high
        packet.data[2] = pid & 0xFF; // PID low
        packet.data[3] = 0x10 | (continuity_counter_ & 0x0F); // No adaptation, payload present, continuity
        
        // Calculate payload size
        size_t payload_offset = 4;
        size_t max_payload = TS_PACKET_SIZE - payload_offset;
        size_t payload_size = std::min(remaining, max_payload);
        
        // Copy payload data
        memcpy(&packet.data[payload_offset], current_data, payload_size);
        
        // Fill remaining bytes with padding if needed
        for (size_t i = payload_offset + payload_size; i < TS_PACKET_SIZE; ++i) {
            packet.data[i] = 0xFF;
        }
        
        packets.push_back(packet);
        
        remaining -= payload_size;
        current_data += payload_size;
        first_packet = false;
        continuity_counter_ = (continuity_counter_ + 1) & 0x0F;
    }
    
    return packets;
}

uint32_t HLSToTSConverter::CalculateCRC32(const uint8_t* data, size_t length) {
    // Simplified CRC32 calculation - in production would use proper CRC32-MPEG
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i] << 24;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ 0x04C11DB7;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void HLSToTSConverter::DetectStreamTypes(TSPacket& packet) {
    // Detect video and audio PIDs by analyzing packet content
    if (packet.payload_unit_start && packet.pid > 0x20) {
        // Look for video/audio stream indicators
        const uint8_t* payload = packet.data + 4;
        size_t payload_size = TS_PACKET_SIZE - 4;
        
        // Check adaptation field
        if ((packet.data[3] & 0x20) != 0 && payload_size > 0) {
            uint8_t adaptation_length = payload[0];
            if (adaptation_length < payload_size) {
                payload += adaptation_length + 1;
                payload_size -= adaptation_length + 1;
            }
        }
        
        // Look for PES header patterns
        if (payload_size >= 6 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            uint8_t stream_id = payload[3];
            
            // Video stream IDs (typically 0xE0-0xEF for MPEG video)
            if ((stream_id >= 0xE0 && stream_id <= 0xEF) || stream_id == 0xBD) {
                packet.is_video_packet = true;
                detected_video_pid_ = packet.pid;
            }
            // Audio stream IDs (typically 0xC0-0xDF for audio)
            else if ((stream_id >= 0xC0 && stream_id <= 0xDF) || stream_id == 0xBD) {
                packet.is_audio_packet = true;
                detected_audio_pid_ = packet.pid;
            }
        }
    } else {
        // Use previously detected PIDs
        if (packet.pid == detected_video_pid_) {
            packet.is_video_packet = true;
        } else if (packet.pid == detected_audio_pid_) {
            packet.is_audio_packet = true;
        }
    }
}

// TransportStreamRouter implementation
TransportStreamRouter::TransportStreamRouter() {
    // Initialize member variables
    player_process_handle_ = INVALID_HANDLE_VALUE;
    
    // Frame Number Tagging: Initialize frame tracking
    total_frames_processed_ = 0;
    frames_dropped_ = 0;
    frames_duplicated_ = 0;
    last_frame_number_ = 0;
    last_frame_time_ = std::chrono::steady_clock::now();
    stream_start_time_ = std::chrono::steady_clock::now();
    
    // Initialize video/audio stream tracking
    video_packets_processed_ = 0;
    audio_packets_processed_ = 0;
    video_frames_processed_ = 0;
    last_video_frame_number_ = 0;
    video_sync_loss_count_ = 0;
    last_video_packet_time_ = std::chrono::steady_clock::now();
    last_audio_packet_time_ = std::chrono::steady_clock::now();
    
    // Initialize MPC workaround state
    is_mpc_player_ = false;
    in_ad_segment_ = false;
    last_video_sync_time_ = std::chrono::steady_clock::now();
    last_key_frame_time_ = std::chrono::steady_clock::now();
    
    // Initialize DirectShow segment event generation state
    schedule_program_restart_ = false;
    program_restart_countdown_ = 0;
    current_pat_version_ = 0;
    current_pmt_version_ = 0;
    pmt_pid_ = 0x1000; // Default PMT PID
    
    // Initialize timing controls
    last_format_change_time_ = std::chrono::steady_clock::now();
    
    // Use dynamic buffer sizing based on system load
    auto& resource_manager = StreamResourceManager::getInstance();
    int active_streams = resource_manager.GetActiveStreamCount();
    
    // Scale buffer size based on active streams to prevent frame drops
    size_t buffer_size = 15000; // Base size ~2.8MB
    if (active_streams > 1) {
        buffer_size = 25000; // ~4.7MB for multiple streams
    }
    if (active_streams > 3) {
        buffer_size = 35000; // ~6.6MB for many streams
    }
    
    ts_buffer_ = std::make_unique<TSBuffer>(buffer_size);
    hls_converter_ = std::make_unique<HLSToTSConverter>();
}

TransportStreamRouter::~TransportStreamRouter() {
    StopRouting();
}

bool TransportStreamRouter::StartRouting(const std::wstring& hls_playlist_url, 
                                        const RouterConfig& config,
                                        std::atomic<bool>& cancel_token,
                                        std::function<void(const std::wstring&)> log_callback) {
    if (routing_active_) {
        return false; // Already routing
    }
    
    current_config_ = config;
    log_callback_ = log_callback;
    routing_active_ = true;
    
    // Detect media player type for workarounds
    DetectMediaPlayerType(config.player_path);
    
    // Reset converter and buffer
    hls_converter_->Reset();
    ts_buffer_->Reset(); // This will clear packets and reset producer_active
    ts_buffer_->SetLowLatencyMode(config.low_latency_mode); // Configure buffer for latency mode
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Starting TSDuck-inspired transport stream routing");
        log_callback_(L"[TS_ROUTER] Player: " + config.player_path);
        log_callback_(L"[TS_ROUTER] Buffer size: " + std::to_wstring(config.buffer_size_packets) + L" packets");
        
        if (config.low_latency_mode) {
            log_callback_(L"[LOW_LATENCY] Mode enabled - targeting minimal stream delay");
            log_callback_(L"[LOW_LATENCY] Max segments: " + std::to_wstring(config.max_segments_to_buffer) + 
                         L", Refresh: " + std::to_wstring(config.playlist_refresh_interval.count()) + L"ms");
        }
    }
    
    // Start HLS fetcher thread
    hls_fetcher_thread_ = std::thread([this, hls_playlist_url, &cancel_token]() {
        HLSFetcherThread(hls_playlist_url, cancel_token);
    });
    
    // Start TS router thread
    ts_router_thread_ = std::thread([this, &cancel_token]() {
        TSRouterThread(cancel_token);
    });
    
    return true;
}

void TransportStreamRouter::StopRouting() {
    if (!routing_active_) return;
    
    routing_active_ = false;
    
    // Signal end of stream to buffer to wake up any waiting threads
    if (ts_buffer_) {
        ts_buffer_->SignalEndOfStream();
    }
    
    // Wait for threads to finish
    if (hls_fetcher_thread_.joinable()) {
        hls_fetcher_thread_.join();
    }
    if (ts_router_thread_.joinable()) {
        ts_router_thread_.join();
    }
    
    // Clear stored process handle since routing has stopped
    player_process_handle_ = INVALID_HANDLE_VALUE;
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Transport stream routing stopped");
    }
}

TransportStreamRouter::BufferStats TransportStreamRouter::GetBufferStats() const {
    BufferStats stats;
    
    stats.buffered_packets = ts_buffer_->GetBufferedPackets();
    stats.total_packets_processed = total_packets_processed_.load();
    
    if (current_config_.buffer_size_packets > 0) {
        stats.buffer_utilization = static_cast<double>(stats.buffered_packets) / current_config_.buffer_size_packets;
    }
    
    // Frame Number Tagging statistics
    stats.total_frames_processed = total_frames_processed_.load();
    stats.frames_dropped = frames_dropped_.load();
    stats.frames_duplicated = frames_duplicated_.load();
    
    // Video/Audio stream statistics
    stats.video_packets_processed = video_packets_processed_.load();
    stats.audio_packets_processed = audio_packets_processed_.load();
    stats.video_frames_processed = video_frames_processed_.load();
    stats.video_sync_loss_count = video_sync_loss_count_.load();
    stats.video_stream_healthy = IsVideoStreamHealthy();
    stats.audio_stream_healthy = IsAudioStreamHealthy();
    
    // Calculate current FPS
    auto now = std::chrono::steady_clock::now();
    auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stream_start_time_);
    if (time_elapsed.count() > 1000 && stats.total_frames_processed > 0) {
        stats.current_fps = static_cast<double>(stats.total_frames_processed * 1000) / time_elapsed.count();
    }
    
    // Calculate average frame interval
    if (stats.total_frames_processed > 1) {
        auto avg_interval_ms = time_elapsed.count() / stats.total_frames_processed;
        stats.avg_frame_interval = std::chrono::milliseconds(avg_interval_ms);
    }
    
    return stats;
}

void TransportStreamRouter::HLSFetcherThread(const std::wstring& playlist_url, std::atomic<bool>& cancel_token) {
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] HLS fetcher thread started");
    }
    
    std::vector<std::wstring> processed_segments;
    bool first_segment = true;
    int consecutive_failures = 0;
    const int max_consecutive_failures = 5;
    
    while (routing_active_ && !cancel_token && consecutive_failures < max_consecutive_failures) {
        try {
            // Fetch playlist
            std::string playlist_content;
            if (!HttpGetText(playlist_url, playlist_content, &cancel_token)) {
                consecutive_failures++;
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Failed to fetch playlist (attempt " + std::to_wstring(consecutive_failures) + L"/" + std::to_wstring(max_consecutive_failures) + L")");
                }
                // Wait before retry, but check cancellation more frequently
                for (int i = 0; i < 20 && routing_active_ && !cancel_token; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Total 2 seconds, but check every 100ms
                }
                continue;
            }
            
            consecutive_failures = 0; // Reset failure counter on success
            
            // Check for stream end before processing segments
            if (playlist_content.find("#EXT-X-ENDLIST") != std::string::npos) {
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Found #EXT-X-ENDLIST - stream ended normally");
                }
                routing_active_ = false;
                break;
            }
            
            // Parse playlist with enhanced discontinuity detection
            tsduck_hls::PlaylistParser playlist_parser;
            std::vector<std::wstring> segment_urls;
            bool has_discontinuities = false;
            
            if (playlist_parser.ParsePlaylist(playlist_content)) {
                // Extract segment URLs from parsed playlist
                auto segments = playlist_parser.GetSegments();
                for (const auto& segment : segments) {
                    segment_urls.push_back(segment.url);
                }
                
                // Check for discontinuities that indicate ad transitions
                has_discontinuities = playlist_parser.HasDiscontinuities();
                
                if (has_discontinuities) {
                    if (log_callback_) {
                        log_callback_(L"[DISCONTINUITY] Detected ad transition - implementing fast restart");
                    }
                    
                    // Clear buffer immediately for fast restart after ad break
                    ts_buffer_->Clear();
                    
                    // Reset frame numbering to prevent frame drop false positives
                    hls_converter_->Reset();
                    
                    // Reset frame statistics to prevent false drop alerts after discontinuity
                    ResetFrameStatistics();
                    
                    if (log_callback_) {
                        log_callback_(L"[FAST_RESTART] Buffer cleared and frame tracking reset for ad transition");
                    }
                    
                    // For fast restart, only process the newest segments
                    if (segment_urls.size() > 1) {
                        // Keep only the last segment for immediate restart
                        std::vector<std::wstring> restart_segments;
                        restart_segments.push_back(segment_urls.back());
                        segment_urls = restart_segments;
                        
                        if (log_callback_) {
                            log_callback_(L"[FAST_RESTART] Using only newest segment for immediate playback");
                        }
                    }
                }
            } else {
                // Fallback to basic parsing if enhanced parser fails
                segment_urls = ParseHLSPlaylist(playlist_content, playlist_url);
            }
            
            if (segment_urls.empty()) {
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] No segments found in playlist");
                }
                // Wait before retry, but check cancellation more frequently
                for (int i = 0; i < 10 && routing_active_ && !cancel_token; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Total 1 second, but check every 100ms
                }
                continue;
            }
            
            // Process new segments with low-latency optimizations
            int segments_processed = 0;
            size_t total_segments = segment_urls.size();
            
            for (size_t i = 0; i < segment_urls.size(); ++i) {
                if (cancel_token || !routing_active_) break;
                
                const auto& segment_url = segment_urls[i];
                
                // Skip already processed segments
                if (std::find(processed_segments.begin(), processed_segments.end(), segment_url) != processed_segments.end()) {
                    continue;
                }
                
                // Low-latency optimization: If we have multiple unprocessed segments and this isn't
                // one of the newest ones, skip it to stay closer to live edge
                if (current_config_.low_latency_mode && current_config_.skip_old_segments) {
                    size_t remaining_segments = total_segments - i;
                    if (remaining_segments > current_config_.max_segments_to_buffer && 
                        i < (total_segments - current_config_.max_segments_to_buffer)) {
                        
                        processed_segments.push_back(segment_url); // Mark as processed to avoid reprocessing
                        if (log_callback_) {
                            log_callback_(L"[LOW_LATENCY] Skipping older segment to maintain live edge");
                        }
                        continue;
                    }
                }
                
                // Fetch segment data
                std::vector<uint8_t> segment_data;
                if (FetchHLSSegment(segment_url, segment_data, &cancel_token)) {
                    if (segment_data.empty()) {
                        if (log_callback_) {
                            log_callback_(L"[TS_ROUTER] Empty segment downloaded: " + segment_url);
                        }
                        continue;
                    }
                    
                    // Check for ad transitions and apply MPC workaround if needed
                    std::string segment_url_utf8 = WideToUtf8(segment_url);
                    bool is_ad_segment = IsAdTransition(segment_url_utf8);
                    if (is_ad_segment != in_ad_segment_) {
                        HandleAdTransition(is_ad_segment);
                    }
                    
                    // Convert to TS packets
                    auto ts_packets = hls_converter_->ConvertSegment(segment_data, first_segment);
                    first_segment = false;
                    
                    if (ts_packets.empty()) {
                        if (log_callback_) {
                            log_callback_(L"[TS_ROUTER] No valid TS packets found in segment");
                        }
                        continue;
                    }
                    
                    // Add to buffer with special handling for post-discontinuity segments
                    size_t buffer_high_watermark, buffer_low_watermark;
                    
                    if (has_discontinuities) {
                        // Immediately after discontinuity: minimal buffering for fastest restart
                        buffer_high_watermark = current_config_.buffer_size_packets / 8; // 12.5% for immediate restart
                        buffer_low_watermark = current_config_.buffer_size_packets / 16;  // 6.25% for fastest response
                        
                        if (log_callback_ && segments_processed == 0) {
                            log_callback_(L"[FAST_RESTART] Using minimal buffering for immediate playback after ad");
                        }
                    } else if (current_config_.low_latency_mode) {
                        // For low-latency, use smaller buffers and more aggressive flow control
                        buffer_high_watermark = current_config_.buffer_size_packets * 6 / 10; // 60% full for low latency
                        buffer_low_watermark = current_config_.buffer_size_packets / 8;       // 12.5% full for faster response
                    } else {
                        // Standard buffering for quality over latency
                        buffer_high_watermark = current_config_.buffer_size_packets * 9 / 10; // 90% full for better buffering
                        buffer_low_watermark = current_config_.buffer_size_packets / 4;       // 25% full for faster recovery
                    }
                    
                    for (const auto& packet_orig : ts_packets) {
                        if (!routing_active_ || cancel_token) break;
                        
                        // Make a copy so we can apply workarounds
                        TSPacket packet = packet_orig;
                        
                        // Apply MPC workaround if enabled
                        if (current_config_.enable_mpc_workaround) {
                            ApplyMPCWorkaround(packet, has_discontinuities && segments_processed == 0);
                        }
                        
                        // Check stream health before adding packets
                        CheckStreamHealth(packet);
                        
                        // Implement flow control with latency-optimized timing
                        while (ts_buffer_->GetBufferedPackets() >= buffer_high_watermark && routing_active_ && !cancel_token) {
                            auto sleep_duration = current_config_.low_latency_mode ? 
                                std::chrono::milliseconds(1) :   // Faster response for low latency
                                std::chrono::milliseconds(2);    // Standard response
                            std::this_thread::sleep_for(sleep_duration);
                        }
                        
                        ts_buffer_->AddPacket(packet);
                        total_packets_processed_++;
                    }
                    
                    processed_segments.push_back(segment_url);
                    segments_processed++;
                    
                    // Keep only recent segments in memory
                    if (processed_segments.size() > 10) {
                        processed_segments.erase(processed_segments.begin());
                    }
                    
                    if (log_callback_ && segments_processed <= 3) { // Log first few segments
                        log_callback_(L"[TS_ROUTER] Processed segment: " + std::to_wstring(ts_packets.size()) + L" TS packets (" + std::to_wstring(segment_data.size()) + L" bytes)");
                    }
                } else {
                    if (log_callback_) {
                        log_callback_(L"[TS_ROUTER] Failed to fetch segment: " + segment_url);
                    }
                }
            }
            
            if (segments_processed > 0 && log_callback_) {
                log_callback_(L"[TS_ROUTER] Batch complete: " + std::to_wstring(segments_processed) + L" new segments processed");
                
                if (current_config_.low_latency_mode) {
                    log_callback_(L"[LOW_LATENCY] Targeting live edge with " + std::to_wstring(current_config_.max_segments_to_buffer) + L" segment buffer");
                }
            }
            
            // Wait before next playlist refresh, use configurable interval for low-latency
            auto refresh_interval = current_config_.low_latency_mode ? 
                current_config_.playlist_refresh_interval : 
                std::chrono::milliseconds(2000);  // Default 2 seconds
                
            // Break the wait into smaller chunks to check cancellation more frequently
            auto chunk_duration = std::chrono::milliseconds(100);
            auto chunks_needed = refresh_interval.count() / chunk_duration.count();
            
            for (int i = 0; i < chunks_needed && routing_active_ && !cancel_token; ++i) {
                std::this_thread::sleep_for(chunk_duration);
            }
            
        } catch (const std::exception& e) {
            consecutive_failures++;
            if (log_callback_) {
                std::string error_msg = e.what();
                log_callback_(L"[TS_ROUTER] HLS fetcher error: " + std::wstring(error_msg.begin(), error_msg.end()));
            }
            // Wait before retry, but check cancellation more frequently  
            for (int i = 0; i < 10 && routing_active_ && !cancel_token; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Total 1 second, but check every 100ms
            }
        }
    }
    
    if (consecutive_failures >= max_consecutive_failures) {
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] HLS fetcher stopping due to too many consecutive failures");
        }
        routing_active_ = false;
    }
    
    // Signal end of stream to TS buffer and router thread
    if (ts_buffer_) {
        ts_buffer_->SignalEndOfStream();
    }
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] HLS fetcher thread stopped");
    }
}

void TransportStreamRouter::TSRouterThread(std::atomic<bool>& cancel_token) {
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] TS router thread started");
    }
    
    HANDLE player_process = INVALID_HANDLE_VALUE;
    HANDLE player_stdin = INVALID_HANDLE_VALUE;
    
    // Launch media player
    if (!LaunchMediaPlayer(current_config_, player_process, player_stdin)) {
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Failed to launch media player");
        }
        routing_active_ = false;
        return;
    }
    
    // Store player process handle for external monitoring
    player_process_handle_ = player_process;
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Media player launched successfully");
    }
    
    size_t packets_sent = 0;
    auto last_log_time = std::chrono::steady_clock::now();
    auto last_packet_time = std::chrono::steady_clock::now();
    
    // Send TS packets to player with natural timing - let the stream flow naturally
    while (routing_active_ && !cancel_token) {
        // Check if player process is still running with better error reporting
        if (player_process != INVALID_HANDLE_VALUE) {
            DWORD exit_code;
            if (GetExitCodeProcess(player_process, &exit_code)) {
                if (exit_code != STILL_ACTIVE) {
                    if (log_callback_) {
                        log_callback_(L"[TS_ROUTER] Media player process exited (code: " + std::to_wstring(exit_code) + L")");
                    }
                    // Set cancel token to signal this was due to player death, not normal completion
                    cancel_token = true;
                    break;
                }
            } else {
                DWORD error = GetLastError();
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Failed to check player process status (error: " + std::to_wstring(error) + L")");
                }
                // If we can't check the process status, it's likely dead - set cancel token
                if (error == ERROR_INVALID_HANDLE) {
                    cancel_token = true;
                    break;
                }
            }
        }
        
        // Get and send packets with timeout optimized for latency mode
        TSPacket packet;
        auto packet_timeout = current_config_.low_latency_mode ? 
            std::chrono::milliseconds(10) :   // Very fast timeout for low latency
            std::chrono::milliseconds(50);    // Standard timeout
            
        if (ts_buffer_->GetNextPacket(packet, packet_timeout)) {
            // Frame Number Tagging: Track frame statistics
            if (packet.frame_number > 0) {
                // Check for frame drops or duplicates
                uint64_t current_frame = packet.frame_number;
                uint64_t last_frame = last_frame_number_.load();
                
                if (current_frame > last_frame + 1) {
                    // Frame drop detected
                    uint32_t dropped = static_cast<uint32_t>(current_frame - last_frame - 1);
                    frames_dropped_ += dropped;
                    
                    if (log_callback_) {
                        log_callback_(L"[FRAME_TAG] Frame drop detected: " + std::to_wstring(dropped) + 
                                     L" frames dropped between #" + std::to_wstring(last_frame) + 
                                     L" and #" + std::to_wstring(current_frame));
                    }
                } else if (current_frame <= last_frame && last_frame > 0) {
                    // Potential duplicate or reordered frame
                    frames_duplicated_++;
                    
                    if (log_callback_) {
                        log_callback_(L"[FRAME_TAG] Duplicate/reordered frame: #" + std::to_wstring(current_frame) + 
                                     L" (last: #" + std::to_wstring(last_frame) + L")");
                    }
                }
                
                last_frame_number_ = current_frame;
                total_frames_processed_++;
                
                // Video-specific frame tracking
                if (packet.is_video_packet) {
                    video_frames_processed_++;
                    last_video_frame_number_ = packet.video_frame_number;
                    
                    // Check for video synchronization issues
                    if (packet.video_sync_lost) {
                        video_sync_loss_count_++;
                        if (log_callback_) {
                            log_callback_(L"[VIDEO_SYNC] Video synchronization lost at frame #" + std::to_wstring(current_frame));
                        }
                    }
                }
                
                // Log frame info for key frames or periodically
                if (packet.is_key_frame || (current_frame % 300 == 0)) { // Every 300 frames or key frames
                    if (log_callback_) {
                        log_callback_(L"[FRAME_TAG] " + packet.GetFrameDebugInfo());
                    }
                }
                
                // Special handling for video stream health
                if (packet.is_video_packet) {
                    auto now = std::chrono::steady_clock::now();
                    auto video_gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_video_packet_time_);
                    
                    // If we haven't seen video packets for more than 5 seconds, log warning
                    if (video_gap.count() > 5000) {
                        if (log_callback_) {
                            log_callback_(L"[VIDEO_HEALTH] Warning: No video packets for " + std::to_wstring(video_gap.count()) + L"ms");
                        }
                    }
                    
                    last_video_packet_time_ = now;
                }
            }
            
            // Send packet normally
            if (!SendTSPacketToPlayer(player_stdin, packet)) {
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Failed to send TS packet to player - pipe may be broken");
                }
                goto cleanup_and_exit;
            }
                    }
                    goto cleanup_and_exit;
                }
            }
            packets_sent++;
            last_packet_time = std::chrono::steady_clock::now();
        } else {
            // No packet available, check if we should continue waiting
            if (!ts_buffer_->IsProducerActive() && ts_buffer_->IsEmpty()) {
                // Stream ended normally and buffer is empty - clean exit
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Stream ended normally - no more packets to send");
                }
                break;
            } else if (ts_buffer_->IsEmpty() && packets_sent == 0) {
                // No packets sent yet and buffer is empty - might be a problem
                static int empty_buffer_warnings = 0;
                if (empty_buffer_warnings < 3) {
                    if (log_callback_) {
                        log_callback_(L"[TS_ROUTER] Waiting for stream data...");
                    }
                    empty_buffer_warnings++;
                }
            }
        }
        
        // Log progress periodically
        if (std::chrono::duration_cast<std::chrono::seconds>(last_packet_time - last_log_time).count() >= 30) {
            if (log_callback_) {
                log_callback_(L"[TS_ROUTER] Streaming progress: " + std::to_wstring(packets_sent) + L" packets sent");
            }
            last_log_time = last_packet_time;
        }
    }
    
cleanup_and_exit:
    // Cleanup
    if (player_stdin != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(player_stdin); // Ensure all data is written
        CloseHandle(player_stdin);
    }
    if (player_process != INVALID_HANDLE_VALUE) {
        // Give player a moment to finish processing
        if (WaitForSingleObject(player_process, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(player_process, 0);
        }
        CloseHandle(player_process);
        // Clear stored handle
        player_process_handle_ = INVALID_HANDLE_VALUE;
    }
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] TS router thread stopped (" + std::to_wstring(packets_sent) + L" packets sent)");
    }
}

bool TransportStreamRouter::LaunchMediaPlayer(const RouterConfig& config, HANDLE& process_handle, HANDLE& stdin_handle) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    
    // Create pipe for stdin with larger buffer for TS data to prevent frame drops
    HANDLE stdin_read, stdin_write;
    auto& resource_manager = StreamResourceManager::getInstance();
    DWORD pipe_buffer_size = resource_manager.GetRecommendedPipeBuffer();
    
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, pipe_buffer_size)) {
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Failed to create pipe for media player");
        }
        return false;
    }
    
    // Make write handle non-inheritable
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    
    // Setup process info
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    PROCESS_INFORMATION pi = {};
    
    // Build command line using the configured player arguments (simple approach like HLS mode)
    std::wstring cmd_line = L"\"" + config.player_path + L"\" " + config.player_args;
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Launching player: " + cmd_line);
    }
    
    // Launch process
    if (!CreateProcessW(nullptr, &cmd_line[0], nullptr, nullptr, TRUE, 
                        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)) {
        DWORD error = GetLastError();
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Failed to launch player process, error: " + std::to_wstring(error));
        }
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        return false;
    }
    
    // Set process priority for better multi-stream performance
    DWORD recommended_priority = resource_manager.GetRecommendedProcessPriority();
    SetPriorityClass(pi.hProcess, recommended_priority);
    
    if (log_callback_) {
        std::wstring priority_name;
        switch (recommended_priority) {
            case HIGH_PRIORITY_CLASS: priority_name = L"HIGH"; break;
            case ABOVE_NORMAL_PRIORITY_CLASS: priority_name = L"ABOVE_NORMAL"; break;
            case NORMAL_PRIORITY_CLASS: priority_name = L"NORMAL"; break;
            default: priority_name = L"UNKNOWN"; break;
        }
        log_callback_(L"[TS_ROUTER] Set " + priority_name + L" priority for media player, active streams: " + 
                     std::to_wstring(resource_manager.GetActiveStreamCount()));
    }
    
    // Cleanup
    CloseHandle(stdin_read);
    CloseHandle(pi.hThread);
    
    process_handle = pi.hProcess;
    stdin_handle = stdin_write;
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Media player process launched successfully");
    }
    
    return true;
}

bool TransportStreamRouter::SendTSPacketToPlayer(HANDLE stdin_handle, const TSPacket& packet) {
    if (stdin_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD bytes_written = 0;
    BOOL write_result = WriteFile(stdin_handle, packet.data, TS_PACKET_SIZE, &bytes_written, nullptr);
    
    if (!write_result) {
        DWORD error = GetLastError();
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] WriteFile failed, error: " + std::to_wstring(error));
        }
        return false;
    }
    
    if (bytes_written != TS_PACKET_SIZE) {
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Partial write: " + std::to_wstring(bytes_written) + L"/" + std::to_wstring(TS_PACKET_SIZE));
        }
        return false;
    }
    
    return true;
}

bool TransportStreamRouter::FetchHLSSegment(const std::wstring& segment_url, std::vector<uint8_t>& data, std::atomic<bool>* cancel_token) {
    // Use binary HTTP download for proper segment data handling
    return HttpGetBinary(segment_url, data, cancel_token);
}

// Validate m3u8 playlist has required metadata tags for proper processing
static bool ValidatePlaylistMetadata(const std::string& playlist, std::function<void(const std::wstring&)> log_callback = nullptr) {
    // Required tags for high-quality Twitch playlists
    std::vector<std::string> required_tags = {
        "#EXTM3U",
        "#EXT-X-VERSION", 
        "#EXT-X-TARGETDURATION",
        "#EXT-X-MEDIA-SEQUENCE",
        "#EXT-X-TWITCH-LIVE-SEQUENCE",
        "#EXT-X-TWITCH-ELAPSED-SECS",
        "#EXT-X-TWITCH-TOTAL-SECS:",
        "#EXT-X-DATERANGE",
        "#EXT-X-PROGRAM-DATE-TIME",
        "#EXTINF"
    };
    
    // First check that all required tags are present
    for (const auto& tag : required_tags) {
        if (playlist.find(tag) == std::string::npos) {
            if (log_callback) {
                log_callback(L"[TS_VALIDATION] Missing required tag: " + Utf8ToWide(tag));
            }
            return false;
        }
    }
    
    // Then check that no extra tags are present
    std::istringstream ss(playlist);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        
        // Skip non-tag lines
        if (line[0] != '#') continue;
        
        // Extract tag name (everything before : or end of line)
        std::string tag_name = line;
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            tag_name = line.substr(0, colon_pos + 1);
        }
        
        // Check if this tag is in the required list
        bool tag_allowed = false;
        for (const auto& required_tag : required_tags) {
            if (tag_name.find(required_tag) == 0) {
                tag_allowed = true;
                break;
            }
        }
        
        if (!tag_allowed) {
            if (log_callback) {
                log_callback(L"[TS_VALIDATION] Found extra tag not allowed: " + Utf8ToWide(tag_name));
            }
            return false;
        }
    }
    
    if (log_callback) {
        log_callback(L"[TS_VALIDATION] Playlist validation passed - only required metadata present");
    }
    return true;
}

std::vector<std::wstring> TransportStreamRouter::ParseHLSPlaylist(const std::string& playlist_content, const std::wstring& base_url) {
    std::vector<std::wstring> segment_urls;
    
    // Validate playlist has required metadata before processing
    if (!ValidatePlaylistMetadata(playlist_content, log_callback_)) {
        if (log_callback_) {
            log_callback_(L"[TS_VALIDATION] Playlist validation failed - skipping this playlist");
        }
        return segment_urls; // Return empty vector
    }
    
    std::istringstream stream(playlist_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // Skip all header/tag lines
            continue;
        }
        
        // This is a segment URL - convert to wide string and join with base URL
        std::wstring rel_url(line.begin(), line.end());
        std::wstring full_url = JoinUrl(base_url, rel_url);
        segment_urls.push_back(full_url);
    }
    
    // For low-latency mode, only return the newest segments (live edge)
    if (current_config_.low_latency_mode && segment_urls.size() > current_config_.max_segments_to_buffer) {
        // Keep only the most recent segments for minimal latency
        size_t start_index = segment_urls.size() - current_config_.max_segments_to_buffer;
        std::vector<std::wstring> live_edge_segments(
            segment_urls.begin() + start_index, 
            segment_urls.end()
        );
        
        if (log_callback_) {
            log_callback_(L"[LOW_LATENCY] Targeting live edge: " + std::to_wstring(live_edge_segments.size()) + 
                         L" of " + std::to_wstring(segment_urls.size()) + L" segments");
        }
        
        return live_edge_segments;
    }
    
    return segment_urls;

}

void TransportStreamRouter::InsertPCR(TSPacket& packet, uint64_t pcr_value) {
    // PCR insertion disabled - HLS segments already have proper timing information
    // Modifying PCR can interfere with the original stream timing and cause playback issues
    // Let the original TS packets pass through unchanged for best compatibility
}

void TransportStreamRouter::ResetFrameStatistics() {
    // Reset all frame tracking statistics after discontinuities
    total_frames_processed_ = 0;
    frames_dropped_ = 0;
    frames_duplicated_ = 0;
    last_frame_number_ = 0;
    last_frame_time_ = std::chrono::steady_clock::now();
    stream_start_time_ = std::chrono::steady_clock::now();
    
    // Reset video/audio stream health tracking
    video_packets_processed_ = 0;
    audio_packets_processed_ = 0;
    video_frames_processed_ = 0;
    last_video_frame_number_ = 0;
    video_sync_loss_count_ = 0;
    last_video_packet_time_ = std::chrono::steady_clock::now();
    last_audio_packet_time_ = std::chrono::steady_clock::now();
}

bool TransportStreamRouter::IsVideoStreamHealthy() const {
    auto now = std::chrono::steady_clock::now();
    auto video_gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_video_packet_time_);
    
    // Consider video healthy if we've seen packets within the last 3 seconds
    return video_gap.count() < 3000 && video_packets_processed_.load() > 0;
}

bool TransportStreamRouter::IsAudioStreamHealthy() const {
    auto now = std::chrono::steady_clock::now();
    auto audio_gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_audio_packet_time_);
    
    // Consider audio healthy if we've seen packets within the last 3 seconds
    return audio_gap.count() < 3000 && audio_packets_processed_.load() > 0;
}

void TransportStreamRouter::CheckStreamHealth(const TSPacket& packet) {
    auto now = std::chrono::steady_clock::now();
    
    if (packet.is_video_packet) {
        video_packets_processed_++;
        last_video_packet_time_ = now;
        
        // Check for video sync issues
        if (!packet.IsVideoSyncValid()) {
            video_sync_loss_count_++;
        }
    } else if (packet.is_audio_packet) {
        audio_packets_processed_++;
        last_audio_packet_time_ = now;
    }
    
    // Log warnings if stream health is degraded
    static auto last_health_check = std::chrono::steady_clock::now();
    auto health_check_interval = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_health_check);
    
    if (health_check_interval.count() > 10000) { // Check every 10 seconds
        if (!IsVideoStreamHealthy() && log_callback_) {
            log_callback_(L"[STREAM_HEALTH] WARNING: Video stream appears unhealthy - possible black frame issue");
        }
        if (!IsAudioStreamHealthy() && log_callback_) {
            log_callback_(L"[STREAM_HEALTH] WARNING: Audio stream appears unhealthy");
        }
        last_health_check = now;
    }
}

// MPC-HC Workaround Implementation
bool TransportStreamRouter::DetectMediaPlayerType(const std::wstring& player_path) {
    std::wstring lowerPath = player_path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);
    
    if (lowerPath.find(L"mpc-hc") != std::wstring::npos ||
        lowerPath.find(L"mpc-be") != std::wstring::npos ||
        lowerPath.find(L"mpc_hc") != std::wstring::npos ||
        lowerPath.find(L"mpc_be") != std::wstring::npos ||
        lowerPath.find(L"mpcbe") != std::wstring::npos ||
        lowerPath.find(L"mpchc") != std::wstring::npos ||
        lowerPath.find(L"vlc") != std::wstring::npos ||
        lowerPath.find(L"potplayer") != std::wstring::npos) {
        is_mpc_player_ = true;
        if (log_callback_) {
            log_callback_(L"[MPC-WORKAROUND] Detected MPC-compatible player: " + player_path);
        }
        return true;
    }
    
    is_mpc_player_ = false;
    return false;
}

void TransportStreamRouter::ApplyMPCWorkaround(TSPacket& packet, bool is_discontinuity) {
    if (!is_mpc_player_ || !current_config_.enable_mpc_workaround) return;
    
    // Apply program restart when scheduled to generate DirectShow segment events
    if (schedule_program_restart_) {
        ApplyProgramRestart(packet);
    }
}


bool TransportStreamRouter::IsAdTransition(const std::string& segment_url) const {
    // Simple ad detection based on URL patterns
    // Twitch ads typically have different URL patterns than content
    std::string lowerUrl = segment_url;
    std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(), ::tolower);
    
    return (lowerUrl.find("ads") != std::string::npos ||
            lowerUrl.find("commercial") != std::string::npos ||
            lowerUrl.find("preroll") != std::string::npos ||
            lowerUrl.find("midroll") != std::string::npos ||
            lowerUrl.find("-ad-") != std::string::npos ||
            lowerUrl.find("_ad_") != std::string::npos);
}

void TransportStreamRouter::HandleAdTransition(bool entering_ad) {
    bool was_in_ad = in_ad_segment_;
    in_ad_segment_ = entering_ad;
    
    if (is_mpc_player_ && current_config_.enable_mpc_workaround) {
        // Apply enhanced workaround when exiting ads (main content resuming)
        // This is when the video buffer freeze typically occurs
        if (!entering_ad && was_in_ad) {
            if (log_callback_) {
                log_callback_(L"[MPC-WORKAROUND] Exiting ad segment - triggering DirectShow segment event for buffer recovery");
            }
            
            // Schedule a program structure change that will generate DirectShow events
            // This mimics what causes EC_SEGMENT_STARTED events that MPC-HC responds to
            schedule_program_restart_ = true;
            program_restart_countdown_ = 3; // Apply over next 3 packets for reliability
            
            // Reset timing to allow immediate action
            last_format_change_time_ = std::chrono::steady_clock::time_point{};
        }
        // Don't do anything when entering ads to avoid unnecessary disruption
    }
}

// DirectShow segment event generation for MPC-HC compatibility
void TransportStreamRouter::ForceDiscontinuityOnNextPackets() {
    // Schedule program structure restart that generates DirectShow events
    // This triggers EC_SEGMENT_STARTED events that MPC-HC buffer flush logic responds to
    schedule_program_restart_ = true;
    program_restart_countdown_ = 3; // Apply over next 3 packets
    
    if (log_callback_) {
        log_callback_(L"[MPC-WORKAROUND] Scheduled program restart to trigger DirectShow segment events");
    }
}

void TransportStreamRouter::ApplyProgramRestart(TSPacket& packet) {
    // Generate a program structure restart that causes DirectShow to emit
    // EC_SEGMENT_STARTED events, which trigger MPC-HC's buffer flush logic
    
    if (program_restart_countdown_ <= 0) {
        schedule_program_restart_ = false;
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    
    // Rate limiting to prevent stream corruption
    if (now < last_format_change_time_ + std::chrono::milliseconds(500)) {
        return; // Too soon since last change
    }
    
    // Apply different aspects of program restart over multiple packets
    bool modified = false;
    
    if (packet.pid == 0x0000) { // PAT packet
        // Increment PAT version to signal program structure change
        if (packet.data[10] & 0x01) { // Check version bit 0
            packet.data[10] &= ~0x3E; // Clear version bits (5 bits)
            packet.data[10] |= ((current_pat_version_ & 0x1F) << 1); // Set new version
        }
        current_pat_version_ = (current_pat_version_ + 1) % 32;
        modified = true;
        
        if (log_callback_) {
            log_callback_(L"[MPC-WORKAROUND] Applied PAT version change for program restart (v" + 
                         std::to_wstring(current_pat_version_) + L")");
        }
    }
    else if (packet.pid == pmt_pid_ && pmt_pid_ != 0) { // PMT packet
        // Increment PMT version to signal program structure change  
        if (packet.data[10] & 0x01) { // Check version bit 0
            packet.data[10] &= ~0x3E; // Clear version bits (5 bits)  
            packet.data[10] |= ((current_pmt_version_ & 0x1F) << 1); // Set new version
        }
        current_pmt_version_ = (current_pmt_version_ + 1) % 32;
        modified = true;
        
        if (log_callback_) {
            log_callback_(L"[MPC-WORKAROUND] Applied PMT version change for program restart (v" + 
                         std::to_wstring(current_pmt_version_) + L")");
        }
    }
    else if (packet.is_video_packet) {
        // Add discontinuity indicator to video packets to complete the segment boundary signal
        if (!(packet.data[3] & 0x20)) {
            // Add minimal adaptation field with discontinuity indicator
            uint8_t old_flags = packet.data[3];
            packet.data[3] = old_flags | 0x20; // Set adaptation field flag
            packet.data[4] = 1; // Adaptation field length = 1 byte  
            packet.data[5] = 0x80; // Set discontinuity indicator bit
            
            // Compact payload if present to make room
            if (old_flags & 0x10) { // Had payload
                for (int i = 186; i >= 6; i--) {
                    packet.data[i] = (i >= 8) ? packet.data[i-2] : 0xFF;
                }
            }
        } else {
            // Set discontinuity bit in existing adaptation field
            if (packet.data[4] > 0) {
                packet.data[5] |= 0x80;
            }
        }
        modified = true;
        
        if (log_callback_) {
            log_callback_(L"[MPC-WORKAROUND] Applied video discontinuity indicator for segment boundary");
        }
    }
    
    if (modified) {
        last_format_change_time_ = now;
        program_restart_countdown_--;
        
        // Recalculate transport stream CRC if needed
        RecalculatePacketCRC(packet);
    }
}

void TransportStreamRouter::RecalculatePacketCRC(TSPacket& packet) {
    // Recalculate CRC32 for PAT/PMT packets after modification
    // This ensures the transport stream remains valid
    
    if (packet.pid == 0x0000 || packet.pid == pmt_pid_) {
        // Find payload start (skip TS header and adaptation field)
        int payload_start = 4;
        if (packet.data[3] & 0x20) { // Has adaptation field
            payload_start += packet.data[4] + 1;
        }
        
        if (payload_start < 188 && (packet.data[3] & 0x10)) { // Has payload
            // Find section length to calculate CRC area
            int section_length = ((packet.data[payload_start + 1] & 0x0F) << 8) | packet.data[payload_start + 2];
            int crc_start = payload_start;
            int crc_end = payload_start + section_length + 3 - 4; // Exclude existing CRC
            
            if (crc_end > 0 && crc_end < 184) {
                // Simple CRC32 calculation for PSI sections
                uint32_t crc = CalculateCRC32(&packet.data[crc_start], crc_end - crc_start);
                
                // Store CRC in big-endian format
                packet.data[crc_end] = (crc >> 24) & 0xFF;
                packet.data[crc_end + 1] = (crc >> 16) & 0xFF;
                packet.data[crc_end + 2] = (crc >> 8) & 0xFF;  
                packet.data[crc_end + 3] = crc & 0xFF;
            }
        }
    }
}

uint32_t TransportStreamRouter::CalculateCRC32(const uint8_t* data, int length) {
    // Simple CRC32 calculation for PSI sections
    // Using standard MPEG CRC32 polynomial: 0x04C11DB7
    
    static const uint32_t crc_table[256] = {
        0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B,
        0x1A864DB2, 0x1E475005, 0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61,
        0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD, 0x4C11DB70, 0x48D0C6C7,
        0x4593E01E, 0x4152FDA9, 0x5F15ADAC, 0x5BD4B01B, 0x569796C2, 0x52568B75,
        0x6A1936C8, 0x6ED82B7F, 0x639B0DA6, 0x675A1011, 0x791D4014, 0x7DDC5DA3,
        0x709F7B7A, 0x745E66CD, 0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039,
        0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5, 0xBE2B5B58, 0xBAEA46EF,
        0xB7A96036, 0xB3687D81, 0xAD2F2D84, 0xA9EE3033, 0xA4AD16EA, 0xA06C0B5D,
        0xD4326D90, 0xD0F37027, 0xDDB056FE, 0xD9714B49, 0xC7361B4C, 0xC3F706FB,
        0xCEB42022, 0xCA753D95, 0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1,
        0xE13EF6F4, 0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D, 0x34867077, 0x30476DC0,
        0x3D044B19, 0x39C556AE, 0x278206AB, 0x23431B1C, 0x2E003DC5, 0x2AC12072,
        0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16, 0x018AEB13, 0x054BF6A4,
        0x0808D07D, 0x0CC9CDCA, 0x7897AB07, 0x7C56B6B0, 0x71159069, 0x75D48DDE,
        0x6B93DDDB, 0x6F52C06C, 0x6211E6B5, 0x66D0FB02, 0x5E9F46BF, 0x5A5E5B08,
        0x571D7DD1, 0x53DC6066, 0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
        0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E, 0xBFA1B04B, 0xBB60ADFC,
        0xB6238B25, 0xB2E29692, 0x8AAD2B2F, 0x8E6C3698, 0x832F1041, 0x87EE0DF6,
        0x99A95DF3, 0x9D684044, 0x902B669D, 0x94EA7B2A, 0xE0B41DE7, 0xE4750050,
        0xE9362689, 0xEDF73B3E, 0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2,
        0xC6BCF05F, 0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686, 0xD5B88683, 0xD1799B34,
        0xDC3ABDED, 0xD8FBA05A, 0x690CE0EE, 0x6DCDFD59, 0x608EDB80, 0x644FC637,
        0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB, 0x4F040D56, 0x4BC510E1,
        0x46863638, 0x42472B8F, 0x5C007B8A, 0x58C1663D, 0x558240E4, 0x51435D53,
        0x251D3B9E, 0x21DC2629, 0x2C9F00F0, 0x285E1D47, 0x36194D42, 0x32D850F5,
        0x3F9B762C, 0x3B5A6B9B, 0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF,
        0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623, 0xF12F560E, 0xF5EE4BB9,
        0xF8AD6D60, 0xFC6C70D7, 0xE22B20D2, 0xE6EA3D65, 0xEBA91BBC, 0xEF68060B,
        0xD727BBB6, 0xD3E6A601, 0xDEA580D8, 0xDA649D6F, 0xC423CD6A, 0xC0E2D0DD,
        0xCDA1F604, 0xC960EBB3, 0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7,
        0xAE3AFBA2, 0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B, 0x9B3660C6, 0x9FF77D71,
        0x92B45BA8, 0x9675461F, 0x8832161A, 0x8CF30BAD, 0x81B02D74, 0x857130C3,
        0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640, 0x4E8EE645, 0x4A4FFBF2,
        0x470CDD2B, 0x43CDC09C, 0x7B827D21, 0x7F436096, 0x7200464F, 0x76C15BF8,
        0x68860BFD, 0x6C47164A, 0x61043093, 0x65C52D24, 0x119B4BE9, 0x155A565E,
        0x18197087, 0x1CD86D30, 0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC,
        0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088, 0x2497D08D, 0x2056CD3A,
        0x2D15EBE3, 0x29D4F654, 0xC5A92679, 0xC1683BCE, 0xCC2B1D17, 0xC8EA00A0,
        0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB, 0xDBEE767C, 0xE3A1CBC1, 0xE760D676,
        0xEA23F0AF, 0xEEE2ED18, 0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4,
        0x89B8FD09, 0x8D79E0BE, 0x803AC667, 0x84FBDBD0, 0x9ABC8BD5, 0x9E7D9662,
        0x933EB0BB, 0x97FFAD0C, 0xAFB010B1, 0xAB710D06, 0xA6322BDF, 0xA2F33668,
        0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4
    };
    
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < length; i++) {
        crc = (crc << 8) ^ crc_table[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}
        packet.data[1] |= 0x40; // Set payload_unit_start_indicator
} // namespace tsduck_transport