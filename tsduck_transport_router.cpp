#include "tsduck_transport_router.h"
#include "twitch_api.h"
#include "playlist_parser.h"
#include "tsduck_hls_wrapper.h"
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

// Ad block state tracking for preventing infinite ad segment skipping
struct AdBlockState {
    bool in_scte35_out = false;
    bool skip_next_segment = false; 
    int consecutive_skipped_segments = 0;
    std::chrono::steady_clock::time_point scte35_start_time;
    bool just_started_ad_block = false; // Flag to indicate we just started an ad block
    bool just_exited_ad_block = false; // Flag to indicate we just exited an ad block
    static const int MAX_CONSECUTIVE_SKIPPED_SEGMENTS = 15; // ~30 seconds for 2-second segments
    static constexpr std::chrono::seconds MAX_AD_BLOCK_DURATION{60}; // 60 seconds max ad block
};

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

// TSBuffer implementation
TSBuffer::TSBuffer(size_t max_packets) : max_packets_(max_packets) {
}

bool TSBuffer::AddPacket(const TSPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (packet_queue_.size() >= max_packets_) {
        // Buffer full - remove oldest packet to make room
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

// HLSToTSConverter implementation
HLSToTSConverter::HLSToTSConverter() {
    Reset();
}

void HLSToTSConverter::Reset() {
    continuity_counter_ = 0;
    pat_sent_ = false;
    pmt_sent_ = false;
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
        
        // TODO: Add TS packet content analysis for ad detection
        // This could include parsing SCTE-35 splice commands within the TS packets
        // for more sophisticated ad detection based on actual packet data rather than
        // just playlist metadata. SCTE-35 splice commands are typically in PID 0x1FFB
        // or other designated PIDs and contain splice_insert() and splice_null() commands.
        
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

// TransportStreamRouter implementation
TransportStreamRouter::TransportStreamRouter() {
    ts_buffer_ = std::make_unique<TSBuffer>(5000);
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
    ts_buffer_->Clear();
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Starting TSDuck-inspired transport stream routing");
        log_callback_(L"[TS_ROUTER] Player: " + config.player_path);
        log_callback_(L"[TS_ROUTER] Buffer size: " + std::to_wstring(config.buffer_size_packets) + L" packets");
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
    
    // Wait for threads to finish
    if (hls_fetcher_thread_.joinable()) {
        hls_fetcher_thread_.join();
    }
    if (ts_router_thread_.joinable()) {
        ts_router_thread_.join();
    }
    
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
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            consecutive_failures = 0; // Reset failure counter on success
            
            // Parse segment URLs
            auto segment_urls = ParseHLSPlaylist(playlist_content, playlist_url);
            
            if (segment_urls.empty()) {
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] No segments found in playlist");
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            
            // Process new segments
            int segments_processed = 0;
            for (const auto& segment_url : segment_urls) {
                if (cancel_token || !routing_active_) break;
                
                // Skip already processed segments
                if (std::find(processed_segments.begin(), processed_segments.end(), segment_url) != processed_segments.end()) {
                    continue;
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
                    
                    // Add to buffer with gentler flow control
                    size_t buffer_high_watermark = current_config_.buffer_size_packets * 4 / 5; // 80% full
                    size_t buffer_low_watermark = current_config_.buffer_size_packets / 5;      // 20% full
                    
                    for (const auto& packet : ts_packets) {
                        if (!routing_active_ || cancel_token) break;
                        
                        // Implement gentler flow control to prevent buffer overflow
                        while (ts_buffer_->GetBufferedPackets() >= buffer_high_watermark && routing_active_ && !cancel_token) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Less aggressive waiting
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
            }
            
            // Wait before next playlist refresh
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
        } catch (const std::exception& e) {
            consecutive_failures++;
            if (log_callback_) {
                std::string error_msg = e.what();
                log_callback_(L"[TS_ROUTER] HLS fetcher error: " + std::wstring(error_msg.begin(), error_msg.end()));
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    if (consecutive_failures >= max_consecutive_failures) {
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] HLS fetcher stopping due to too many consecutive failures");
        }
        routing_active_ = false;
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
                    break;
                }
            } else {
                DWORD error = GetLastError();
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Failed to check player process status (error: " + std::to_wstring(error) + L")");
                }
            }
        }
        
        // Get and send packets as they become available
        TSPacket packet;
        if (ts_buffer_->GetNextPacket(packet, std::chrono::milliseconds(100))) {
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
            if (ts_buffer_->IsEmpty() && packets_sent == 0) {
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
    }
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] TS router thread stopped (" + std::to_wstring(packets_sent) + L" packets sent)");
    }
}

bool TransportStreamRouter::LaunchMediaPlayer(const RouterConfig& config, HANDLE& process_handle, HANDLE& stdin_handle) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    
    // Create pipe for stdin with moderate buffer for TS data
    HANDLE stdin_read, stdin_write;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 65536)) { // 64KB buffer for good balance
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
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // Hide console window for cleaner experience
    si.hStdInput = stdin_read;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    PROCESS_INFORMATION pi = {};
    
    // Build command line with appropriate arguments for transport stream
    std::wstring cmd_line;
    std::wstring player_name = config.player_path;
    
    // Extract just the executable name for player-specific handling
    size_t last_slash = player_name.find_last_of(L"\\/");
    if (last_slash != std::wstring::npos) {
        player_name = player_name.substr(last_slash + 1);
    }
    std::transform(player_name.begin(), player_name.end(), player_name.begin(), ::towlower);
    
    // Configure player-specific arguments for transport stream input
    if (player_name.find(L"mpv") != std::wstring::npos) {
        // MPV specific arguments for transport stream - respect natural timing
        cmd_line = config.player_path + L" --demuxer=mpegts --cache-secs=1 --no-resume-playback --";
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Using MPV-specific TS arguments with natural timing");
        }
    }
    else if (player_name.find(L"mpc") != std::wstring::npos || player_name.find(L"mphc") != std::wstring::npos) {
        // MPC-HC specific arguments for transport stream - simplified
        cmd_line = config.player_path + L" /play /close \"-\"";
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Using MPC-HC arguments");
        }
    }
    else if (player_name.find(L"vlc") != std::wstring::npos) {
        // VLC specific arguments for transport stream - simplified
        cmd_line = config.player_path + L" --demux=ts --intf=dummy --play-and-exit --network-caching=500 -";
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Using VLC TS arguments");
        }
    }
    else {
        // Generic player arguments (fallback)
        cmd_line = config.player_path + L" " + config.player_args;
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Using generic player arguments");
        }
    }
    
    if (log_callback_) {
        log_callback_(L"[TS_ROUTER] Launching player: " + cmd_line);
    }
    
    // Launch process
    if (!CreateProcessW(nullptr, &cmd_line[0], nullptr, nullptr, TRUE, 
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD error = GetLastError();
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Failed to launch player process, error: " + std::to_wstring(error));
        }
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        return false;
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

std::vector<std::wstring> TransportStreamRouter::ParseHLSPlaylist(const std::string& playlist_content, const std::wstring& base_url) {
    static AdBlockState ad_state; // Persistent state across playlist refreshes
    std::vector<std::wstring> segment_urls;
    
    // First run TSDuck analysis to get sophisticated ad detection results
    tsduck_hls::PlaylistParser tsduck_parser;
    bool tsduck_available = tsduck_parser.ParsePlaylist(playlist_content);
    std::vector<tsduck_hls::MediaSegment> tsduck_segments;
    if (tsduck_available) {
        tsduck_segments = tsduck_parser.GetSegments();
        if (log_callback_) {
            log_callback_(L"[TS_AD_SKIP] TSDuck analysis found " + std::to_wstring(tsduck_segments.size()) + 
                         L" segments, " + std::to_wstring(tsduck_parser.HasAdMarkers() ? 1 : 0) + L" ad markers detected");
        }
    }
    
    // Don't clear buffer when exiting ad blocks to maintain stream continuity
    if (ad_state.just_exited_ad_block) {
        ad_state.just_exited_ad_block = false; // Reset flag
        if (log_callback_) {
            log_callback_(L"[TS_AD_SKIP] Just exited ad block, continuing with existing buffer to maintain continuity");
        }
    }
    
    std::istringstream stream(playlist_content);
    std::string line;
    int current_segment_index = 0; // Track which segment we're processing for TSDuck correlation
    
    while (std::getline(stream, line)) {
        // Remove trailing whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // SCTE-35 ad marker detection (start of ad block)
            if (line.find("#EXT-X-SCTE35-OUT") == 0) {
                if (!ad_state.in_scte35_out) {
                    // Just entering ad block - don't clear buffer to maintain continuity
                    if (log_callback_) {
                        log_callback_(L"[TS_AD_SKIP] Found SCTE35-OUT marker, entering ad block - maintaining buffer continuity");
                    }
                } else {
                    if (log_callback_) {
                        log_callback_(L"[TS_AD_SKIP] Found SCTE35-OUT marker, already in ad block");
                    }
                }
                ad_state.in_scte35_out = true;
                ad_state.consecutive_skipped_segments = 0;
                ad_state.scte35_start_time = std::chrono::steady_clock::now();
                continue;
            }
            // SCTE-35 ad marker detection (end of ad block)
            else if (line.find("#EXT-X-SCTE35-IN") == 0) {
                ad_state.in_scte35_out = false;
                ad_state.consecutive_skipped_segments = 0;
                ad_state.just_exited_ad_block = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Found SCTE35-IN marker, exiting ad block - maintaining buffer continuity");
                }
                continue;
            }
            // Stitched ad segments (single segment ads)
            else if (line.find("stitched-ad") != std::string::npos) {
                ad_state.skip_next_segment = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Found stitched-ad marker");
                }
            }
            // EXTINF tags with specific durations that indicate ads (2.001, 2.002 seconds)
            else if (line.find("#EXTINF:2.00") == 0 && 
                     (line.find("2.001") != std::string::npos || 
                      line.find("2.002") != std::string::npos)) {
                ad_state.skip_next_segment = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Found ad-duration EXTINF marker");
                }
            }
            // DATERANGE markers for stitched ads
            else if (line.find("#EXT-X-DATERANGE:ID=\"stitched-ad") == 0) {
                ad_state.skip_next_segment = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Found stitched-ad DATERANGE marker");
                }
            }
            // General stitched content detection
            else if (line.find("stitched") != std::string::npos ||
                     line.find("STITCHED") != std::string::npos) {
                ad_state.skip_next_segment = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Found general stitched content marker");
                }
            }
            // MIDROLL ad markers
            else if (line.find("EXT-X-DATERANGE") != std::string::npos && 
                     (line.find("MIDROLL") != std::string::npos ||
                      line.find("midroll") != std::string::npos)) {
                ad_state.skip_next_segment = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Found MIDROLL ad marker");
                }
            }
            continue;
        }
        
        // This is a segment URL
        bool should_skip_ad_segment = false;
        
        // First check TSDuck ad detection results if available
        if (tsduck_available && current_segment_index < static_cast<int>(tsduck_segments.size())) {
            const auto& tsduck_segment = tsduck_segments[current_segment_index];
            if (tsduck_segment.is_ad_segment) {
                should_skip_ad_segment = true;
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] TSDuck detected ad segment: " + std::wstring(line.begin(), line.end()));
                }
            }
        }
        
        // Fallback to legacy ad detection logic if TSDuck didn't detect an ad
        if (!should_skip_ad_segment) {
            // Check if we should skip this segment as an ad using legacy detection
            if (ad_state.skip_next_segment) {
                // Single ad segment (stitched ads, duration-based ads, etc.)
                should_skip_ad_segment = true;
                ad_state.skip_next_segment = false; // Reset after processing one segment
            } else if (ad_state.in_scte35_out) {
                // SCTE35 ad block - check safety limits to prevent infinite ad segment skipping
                auto current_time = std::chrono::steady_clock::now();
                auto ad_block_duration = current_time - ad_state.scte35_start_time;
                
                if (ad_state.consecutive_skipped_segments >= AdBlockState::MAX_CONSECUTIVE_SKIPPED_SEGMENTS) {
                    if (log_callback_) {
                        log_callback_(L"[TS_AD_SKIP] Safety limit reached: " + std::to_wstring(ad_state.consecutive_skipped_segments) + 
                                     L" consecutive skipped segments, exiting ad block");
                    }
                    ad_state.in_scte35_out = false;
                    ad_state.consecutive_skipped_segments = 0;
                    ad_state.just_exited_ad_block = true;
                } else if (ad_block_duration >= AdBlockState::MAX_AD_BLOCK_DURATION) {
                    if (log_callback_) {
                        log_callback_(L"[TS_AD_SKIP] Safety timeout reached: " + 
                                     std::to_wstring(std::chrono::duration_cast<std::chrono::seconds>(ad_block_duration).count()) + 
                                     L" seconds in ad block, exiting");
                    }
                    ad_state.in_scte35_out = false;
                    ad_state.consecutive_skipped_segments = 0;
                    ad_state.just_exited_ad_block = true;
                } else {
                    should_skip_ad_segment = true;
                }
            }
        }
        
        if (should_skip_ad_segment) {
            // Ad segment detected - skip it entirely for transport stream mode
            if (log_callback_) {
                log_callback_(L"[TS_AD_SKIP] Skipping ad segment (skipped #" + 
                             std::to_wstring(ad_state.consecutive_skipped_segments + 1) + L"): " + 
                             std::wstring(line.begin(), line.end()));
            }
            ad_state.consecutive_skipped_segments++;
            // Don't add this segment to the list - it will be completely skipped
        } else {
            // Normal content segment - reset skip counter and add to list
            if (ad_state.consecutive_skipped_segments > 0) {
                if (log_callback_) {
                    log_callback_(L"[TS_AD_SKIP] Returning to normal content after skipping " + 
                                 std::to_wstring(ad_state.consecutive_skipped_segments) + L" ad segments");
                }
                ad_state.consecutive_skipped_segments = 0;
            }
            
            // Convert to wide string and join with base URL
            std::wstring rel_url(line.begin(), line.end());
            std::wstring full_url = JoinUrl(base_url, rel_url);
            segment_urls.push_back(full_url);
        }
        
        current_segment_index++; // Increment for TSDuck correlation
    }
    
    return segment_urls;
}

void TransportStreamRouter::InsertPCR(TSPacket& packet, uint64_t pcr_value) {
    // PCR insertion disabled - HLS segments already have proper timing information
    // Modifying PCR can interfere with the original stream timing and cause playback issues
    // Let the original TS packets pass through unchanged for best compatibility
}



} // namespace tsduck_transport