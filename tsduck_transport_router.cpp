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
    
    // Initialize discontinuity-based ad detection state
    last_discontinuity_time_ = std::chrono::steady_clock::now();
    has_recent_discontinuity_ = false;
    
    // Initialize video/audio stream tracking
    video_packets_processed_ = 0;
    audio_packets_processed_ = 0;
    video_frames_processed_ = 0;
    last_video_frame_number_ = 0;
    video_sync_loss_count_ = 0;
    last_video_packet_time_ = std::chrono::steady_clock::now();
    last_audio_packet_time_ = std::chrono::steady_clock::now();
    
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
    
    // Initialize current URL for dynamic switching
    current_playlist_url_ = hls_playlist_url;
    url_switch_pending_ = false;
    
    // Debug: Log ad switching configuration
    if (log_callback_ && current_config_.is_in_ad_mode && current_config_.quality_to_url_map) {
        log_callback_(L"[AD_SYSTEM] Ad-based quality switching initialized");
        log_callback_(L"[AD_SYSTEM] Ad quality: " + current_config_.ad_mode_quality);
        log_callback_(L"[AD_SYSTEM] User quality: " + current_config_.user_quality);
        log_callback_(L"[AD_SYSTEM] Available qualities: " + std::to_wstring(current_config_.quality_to_url_map->size()));
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
            // Check for URL switch
            std::wstring current_url;
            {
                std::lock_guard<std::mutex> lock(url_switch_mutex_);
                current_url = current_playlist_url_;
                if (url_switch_pending_) {
                    // Clear processed segments when switching URLs to force reprocessing
                    processed_segments.clear();
                    first_segment = true;
                    url_switch_pending_ = false;
                    
                    if (log_callback_) {
                        log_callback_(L"[URL_SWITCH] Applied URL switch to: " + current_url);
                    }
                }
            }
            
            // Fetch playlist
            std::string playlist_content;
            if (!HttpGetText(current_url, playlist_content, &cancel_token)) {
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
                
                // Check for ad transitions using discontinuity detection
                bool ad_start_detected = false;
                bool ad_end_detected = false;
                
                // Debug: Log segment count and ad detection status
                if (log_callback_ && segments.size() > 0) {
                    log_callback_(L"[AD_DEBUG] Checking " + std::to_wstring(segments.size()) + L" segments for discontinuities");
                }
                
                // Check current time for ad end detection
                auto now = std::chrono::steady_clock::now();
                bool has_discontinuity_in_batch = false;
                
                for (const auto& segment : segments) {
                    if (playlist_parser.DetectAdStart(segment)) {
                        ad_start_detected = true;
                        has_discontinuity_in_batch = true;
                        last_discontinuity_time_ = now;
                        has_recent_discontinuity_ = true;
                        
                        if (log_callback_) {
                            log_callback_(L"[AD_DETECTION] Discontinuity detected in segment: " + segment.url);
                        }
                    }
                    
                    segment_urls.push_back(segment.url);
                }
                
                // Check if we should end ad mode (no discontinuities for timeout period)
                if (has_recent_discontinuity_ && !has_discontinuity_in_batch) {
                    auto time_since_last_discontinuity = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_discontinuity_time_);
                    if (time_since_last_discontinuity >= discontinuity_timeout_) {
                        ad_end_detected = true;
                        has_recent_discontinuity_ = false;
                        
                        if (log_callback_) {
                            log_callback_(L"[AD_DETECTION] Ad end detected - no discontinuities for " + 
                                        std::to_wstring(time_since_last_discontinuity.count()) + L"ms");
                        }
                    }
                }
                
                // Debug: Log overall ad detection result
                if (log_callback_ && segments.size() > 0) {
                    log_callback_(L"[AD_DEBUG] Ad detection complete - Start: " + 
                                std::wstring(ad_start_detected ? L"YES" : L"NO") + L", End: " + 
                                std::wstring(ad_end_detected ? L"YES" : L"NO") + 
                                L", Recent discontinuity: " + std::wstring(has_recent_discontinuity_ ? L"YES" : L"NO"));
                }
                
                // Handle ad state transitions
                if (current_config_.is_in_ad_mode && current_config_.needs_switch_to_ad && 
                    current_config_.needs_switch_to_user && current_config_.quality_to_url_map &&
                    !current_config_.ad_mode_quality.empty() && !current_config_.user_quality.empty()) {
                    
                    bool currently_in_ad = current_config_.is_in_ad_mode->load();
                    
                    if (ad_start_detected && !currently_in_ad) {
                        // Switching to ad mode
                        current_config_.is_in_ad_mode->store(true);
                        current_config_.needs_switch_to_ad->store(true);
                        
                        if (log_callback_) {
                            log_callback_(L"[AD_MODE] Switching to ad quality: " + current_config_.ad_mode_quality);
                        }
                        
                        // Find the ad quality URL and switch to it
                        auto ad_url_it = current_config_.quality_to_url_map->find(current_config_.ad_mode_quality);
                        if (ad_url_it != current_config_.quality_to_url_map->end()) {
                            // Actually switch to the ad quality URL
                            SwitchPlaylistURL(ad_url_it->second);
                            
                            if (log_callback_) {
                                log_callback_(L"[AD_MODE] Switched to ad URL: " + ad_url_it->second);
                            }
                        } else {
                            if (log_callback_) {
                                log_callback_(L"[AD_MODE] ERROR: Ad quality '" + current_config_.ad_mode_quality + L"' not found in quality map");
                            }
                        }
                    }
                    else if (ad_end_detected && currently_in_ad) {
                        // Switching back to user quality
                        current_config_.is_in_ad_mode->store(false);
                        current_config_.needs_switch_to_user->store(true);
                        
                        if (log_callback_) {
                            log_callback_(L"[AD_MODE] Switching back to user quality: " + current_config_.user_quality);
                        }
                        
                        // Find the user quality URL and switch back to it
                        auto user_url_it = current_config_.quality_to_url_map->find(current_config_.user_quality);
                        if (user_url_it != current_config_.quality_to_url_map->end()) {
                            // Actually switch back to the user quality URL
                            SwitchPlaylistURL(user_url_it->second);
                            
                            if (log_callback_) {
                                log_callback_(L"[AD_MODE] Switched back to user URL: " + user_url_it->second);
                            }
                        } else {
                            if (log_callback_) {
                                log_callback_(L"[AD_MODE] ERROR: User quality '" + current_config_.user_quality + L"' not found in quality map");
                            }
                        }
                    }
                } else if (ad_start_detected || ad_end_detected) {
                    // Ad markers detected but quality switching not properly configured
                    if (log_callback_) {
                        std::wstring config_status = L"[AD_CONFIG_DEBUG] Quality switching configuration - ";
                        config_status += L"is_in_ad_mode: " + std::wstring(current_config_.is_in_ad_mode ? L"SET" : L"NULL") + L", ";
                        config_status += L"needs_switch_to_ad: " + std::wstring(current_config_.needs_switch_to_ad ? L"SET" : L"NULL") + L", ";
                        config_status += L"needs_switch_to_user: " + std::wstring(current_config_.needs_switch_to_user ? L"SET" : L"NULL") + L", ";
                        config_status += L"quality_to_url_map: " + std::wstring(current_config_.quality_to_url_map ? L"SET" : L"NULL") + L", ";
                        config_status += L"ad_mode_quality: '" + current_config_.ad_mode_quality + L"', ";
                        config_status += L"user_quality: '" + current_config_.user_quality + L"'";
                        log_callback_(config_status);
                        
                        log_callback_(L"[AD_DETECTION] SCTE-35 markers detected but quality switching is not properly configured");
                    }
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
                segment_urls = ParseHLSPlaylist(playlist_content, current_url);
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
                    
                    for (const auto& packet : ts_packets) {
                        if (!routing_active_ || cancel_token) break;
                        
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
            
            if (!SendTSPacketToPlayer(player_stdin, packet)) {
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Failed to send TS packet to player - pipe may be broken");
                }
                goto cleanup_and_exit;
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

void TransportStreamRouter::SwitchPlaylistURL(const std::wstring& new_url) {
    if (log_callback_) {
        log_callback_(L"[URL_SWITCH] Switching playlist URL to: " + new_url);
    }
    
    std::lock_guard<std::mutex> lock(url_switch_mutex_);
    current_playlist_url_ = new_url;
    url_switch_pending_ = true;
    
    // Clear buffer to ensure clean transition to new quality
    if (ts_buffer_) {
        ts_buffer_->Clear();
        if (log_callback_) {
            log_callback_(L"[URL_SWITCH] Cleared buffer for clean quality transition");
        }
    }
    
    // Reset converter for clean stream restart
    if (hls_converter_) {
        hls_converter_->Reset();
        if (log_callback_) {
            log_callback_(L"[URL_SWITCH] Reset converter for new quality stream");
        }
    }
    
    if (log_callback_) {
        log_callback_(L"[URL_SWITCH] URL switch queued, will take effect on next playlist fetch");
    }
}

} // namespace tsduck_transport