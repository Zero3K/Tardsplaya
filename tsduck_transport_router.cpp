#include "tsduck_transport_router.h"
#include "twitch_api.h"
#include "playlist_parser.h"
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
    
    // Calculate target packet rate for smooth delivery
    // Use burst-based timing: send bursts with reasonable intervals for TS streams
    const auto target_packet_interval = std::chrono::microseconds(1500); // 1.5ms between bursts of 7 packets
    
    // Send TS packets to player with proper timing
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
        
        // Send multiple packets in small bursts for better TS timing
        std::vector<TSPacket> packet_burst;
        const int burst_size = 7; // Send 7 packets per burst (typical TS behavior)
        
        // Collect packets for burst
        for (int i = 0; i < burst_size && routing_active_ && !cancel_token; ++i) {
            TSPacket packet;
            if (ts_buffer_->GetNextPacket(packet, std::chrono::milliseconds(10))) {
                packet_burst.push_back(packet);
            } else {
                break; // No more packets available
            }
        }
        
        // Send the burst of packets
        for (const auto& packet : packet_burst) {
            if (!SendTSPacketToPlayer(player_stdin, packet)) {
                if (log_callback_) {
                    log_callback_(L"[TS_ROUTER] Failed to send TS packet to player - pipe may be broken");
                }
                goto cleanup_and_exit;
            }
            packets_sent++;
        }
        
        // Rate limiting after burst
        if (!packet_burst.empty()) {
            last_packet_time = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(target_packet_interval);
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
        // MPV specific arguments for transport stream - optimized for live streaming
        cmd_line = config.player_path + L" --demuxer=mpegts --no-cache --untimed --no-correct-pts --framedrop=vo --no-resume-playback --";
        if (log_callback_) {
            log_callback_(L"[TS_ROUTER] Using MPV-specific TS arguments for live streaming");
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
    std::vector<std::wstring> segment_urls;
    
    std::istringstream stream(playlist_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // Remove trailing whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        
        if (line.empty() || line[0] == '#') {
            continue; // Skip empty lines and comments
        }
        
        // This is a segment URL - convert to wide string and join with base URL
        std::wstring rel_url(line.begin(), line.end());
        std::wstring full_url = JoinUrl(base_url, rel_url);
        segment_urls.push_back(full_url);
    }
    
    return segment_urls;
}

void TransportStreamRouter::InsertPCR(TSPacket& packet, uint64_t pcr_value) {
    // Only insert PCR into video PID packets for timing reference
    if (packet.pid != 0x1001) return; // Video PID
    
    // Check if packet has adaptation field or needs one
    bool has_adaptation = (packet.data[3] & 0x20) != 0;
    bool has_payload = (packet.data[3] & 0x10) != 0;
    
    if (!has_adaptation) {
        // Need to create adaptation field for PCR
        
        // Shift payload data to make room for adaptation field
        size_t payload_start = 4;
        size_t available_space = TS_PACKET_SIZE - payload_start;
        size_t adaptation_size = 8; // Minimum size for PCR
        
        if (has_payload && available_space > adaptation_size) {
            // Move payload data
            size_t payload_size = available_space - adaptation_size - 1; // -1 for adaptation length byte
            memmove(&packet.data[payload_start + adaptation_size + 1], 
                   &packet.data[payload_start], payload_size);
            
            // Update adaptation field control bits
            packet.data[3] = (packet.data[3] & 0xCF) | 0x30; // Both adaptation and payload
            
            // Set adaptation field length
            packet.data[4] = adaptation_size - 1; // Length excludes the length byte itself
            
            // Set PCR flag in adaptation field
            packet.data[5] = 0x10; // PCR flag set
            
            // Insert PCR value (33-bit base + 6-bit extension = 48 bits total)
            // PCR_base is 33 bits, PCR_ext is 9 bits (only 6 used)
            uint64_t pcr_base = pcr_value / 300; // Convert to 27MHz base
            uint16_t pcr_ext = pcr_value % 300;   // Remainder for extension
            
            // Pack PCR into 6 bytes (48 bits)
            packet.data[6] = (pcr_base >> 25) & 0xFF;          // bits 32-25
            packet.data[7] = (pcr_base >> 17) & 0xFF;          // bits 24-17  
            packet.data[8] = (pcr_base >> 9) & 0xFF;           // bits 16-9
            packet.data[9] = (pcr_base >> 1) & 0xFF;           // bits 8-1
            packet.data[10] = ((pcr_base & 0x01) << 7) |       // bit 0
                             0x7E |                            // reserved bits
                             ((pcr_ext >> 8) & 0x01);         // PCR_ext bit 8
            packet.data[11] = pcr_ext & 0xFF;                  // PCR_ext bits 7-0
            
            // Fill remaining adaptation field with padding
            for (size_t i = 12; i < payload_start + adaptation_size; ++i) {
                packet.data[i] = 0xFF;
            }
        }
    } else {
        // Adaptation field exists, check if we can insert PCR
        uint8_t adaptation_length = packet.data[4];
        if (adaptation_length >= 7) { // Need at least 7 bytes for PCR
            // Set PCR flag
            packet.data[5] |= 0x10;
            
            // Insert PCR value 
            uint64_t pcr_base = pcr_value / 300;
            uint16_t pcr_ext = pcr_value % 300;
            
            packet.data[6] = (pcr_base >> 25) & 0xFF;
            packet.data[7] = (pcr_base >> 17) & 0xFF;
            packet.data[8] = (pcr_base >> 9) & 0xFF;
            packet.data[9] = (pcr_base >> 1) & 0xFF;
            packet.data[10] = ((pcr_base & 0x01) << 7) | 0x7E | ((pcr_ext >> 8) & 0x01);
            packet.data[11] = pcr_ext & 0xFF;
        }
    }
}



} // namespace tsduck_transport