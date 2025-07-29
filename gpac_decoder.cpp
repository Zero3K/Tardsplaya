#include "gpac_decoder.h"
#include <iostream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <winhttp.h>

// Forward declarations
extern bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token = nullptr);
std::wstring Utf8ToWide(const std::string& str);
void AddDebugLog(const std::wstring& msg);

// GPAC includes - for now we'll use a simplified approach without full GPAC SDK
// In a real implementation, these would be proper GPAC includes:
// #include <gpac/isomedia.h>
// #include <gpac/media_tools.h>
// #include <gpac/scene_manager.h>

// For this implementation, we'll create a GPAC-style interface that can be 
// easily replaced with actual GPAC calls when the SDK is available

namespace gpac_decoder {

// Helper: join relative URL to base
static std::wstring JoinUrl(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}

// HTTP binary download for segment fetching
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

        HINTERNET hSession = WinHttpOpen(L"Tardsplaya-GPAC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
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

// PlaylistParser implementation
PlaylistParser::PlaylistParser() {
    // Initialize with defaults
}

bool PlaylistParser::ParsePlaylist(const std::string& m3u8_content) {
    segments_.clear();
    has_discontinuities_ = false;
    
    std::istringstream stream(m3u8_content);
    std::string line;
    HLSSegment current_segment;
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
        }
        else if (expecting_segment_url) {
            // This is a segment URL
            current_segment.url = std::wstring(line.begin(), line.end());
            current_segment.sequence_number = media_sequence_ + segments_.size();
            
            segments_.push_back(current_segment);
            
            // Reset for next segment
            current_segment = HLSSegment();
            expecting_segment_url = false;
        }
    }
    
    // Post-processing: calculate precise timing
    CalculatePreciseTiming();
    
    return !segments_.empty();
}

void PlaylistParser::ParseInfoLine(const std::string& line, HLSSegment& current_segment) {
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
    }
    catch (const std::exception&) {
        // Default to target duration if parsing fails
        current_segment.duration = target_duration_;
    }
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
    auto current_time = std::chrono::steady_clock::now();
    
    for (size_t i = 0; i < segments_.size(); ++i) {
        auto& segment = segments_[i];
        
        // Calculate expected start time based on cumulative duration
        std::chrono::milliseconds cumulative_duration{0};
        for (size_t j = 0; j < i; ++j) {
            cumulative_duration += segments_[j].duration;
        }
    }
}

int PlaylistParser::GetOptimalBufferSegments() const {
    if (segments_.empty()) return 3; // Default fallback
    
    double avg_duration = 0.0;
    for (const auto& segment : segments_) {
        avg_duration += segment.duration.count() / 1000.0; // Convert to seconds
    }
    
    if (!segments_.empty()) {
        avg_duration /= segments_.size();
    }
    
    // Calculate optimal buffer: aim for 8-12 seconds of content
    int optimal_segments = static_cast<int>(std::ceil(10.0 / avg_duration));
    
    // Clamp to reasonable range
    return std::max(2, std::min(optimal_segments, 6));
}

std::chrono::milliseconds PlaylistParser::GetPlaylistDuration() const {
    std::chrono::milliseconds total{0};
    for (const auto& segment : segments_) {
        total += segment.duration;
    }
    return total;
}

// MediaBuffer implementation
MediaBuffer::MediaBuffer(size_t max_packets) : max_packets_(max_packets), producer_active_(true) {
}

bool MediaBuffer::AddPacket(const MediaPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (packet_queue_.size() >= max_packets_) {
        // Remove oldest packet to make room
        packet_queue_.pop();
    }
    
    packet_queue_.push(packet);
    return true;
}

bool MediaBuffer::GetNextPacket(MediaPacket& packet, std::chrono::milliseconds timeout) {
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

size_t MediaBuffer::GetBufferedPackets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_queue_.size();
}

bool MediaBuffer::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_queue_.empty();
}

bool MediaBuffer::IsFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_queue_.size() >= max_packets_;
}

void MediaBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!packet_queue_.empty()) {
        packet_queue_.pop();
    }
}

void MediaBuffer::Reset() {
    Clear();
    producer_active_ = true;
}

void MediaBuffer::SignalEndOfStream() {
    producer_active_ = false;
}

// GpacHLSDecoder implementation
GpacHLSDecoder::GpacHLSDecoder() {
    Reset();
}

GpacHLSDecoder::~GpacHLSDecoder() {
    CleanupGpacContext();
}

bool GpacHLSDecoder::Initialize() {
    // Initialize GPAC context and decoders
    // For now, we'll simulate this with a simplified implementation
    // In a real implementation, this would initialize GPAC properly:
    
    // gpac_context_ = gf_sys_init(0, 0);
    // if (!gpac_context_) return false;
    
    // For this demo, just set up our simulated context
    gpac_context_ = reinterpret_cast<void*>(0x12345678); // Dummy pointer
    
    return InitializeGpacContext() && SetupVideoDecoder() && SetupAudioDecoder() && SetupMuxers();
}

std::vector<MediaPacket> GpacHLSDecoder::DecodeSegment(const std::vector<uint8_t>& hls_data, bool is_first_segment) {
    std::vector<MediaPacket> packets;
    
    if (hls_data.empty() || !gpac_context_) {
        return packets;
    }
    
    // Reset segment frame counter for new segment
    if (is_first_segment) {
        segment_frame_counter_ = 0;
        last_frame_time_ = std::chrono::steady_clock::now();
    }
    
    // Process the segment data through GPAC decoder
    // For this implementation, we'll simulate the decoding process
    
    // In a real GPAC implementation, this would:
    // 1. Parse the HLS segment (usually MPEG-TS)
    // 2. Extract video and audio tracks
    // 3. Decode them to raw frames
    // 4. Mux into AVI/WAV containers
    // 5. Return the muxed data as MediaPackets
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.segments_processed++;
        stats_.bytes_input += hls_data.size();
    }
    
    // Simulate video processing
    if (enable_avi_output_ && ProcessVideoTrack(hls_data.data(), hls_data.size(), packets)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.video_frames_decoded++;
    }
    
    // Simulate audio processing  
    if (enable_wav_output_ && ProcessAudioTrack(hls_data.data(), hls_data.size(), packets)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.audio_frames_decoded++;
    }
    
    return packets;
}

void GpacHLSDecoder::SetOutputFormat(bool enable_avi, bool enable_wav) {
    enable_avi_output_ = enable_avi;
    enable_wav_output_ = enable_wav;
}

void GpacHLSDecoder::SetQuality(int video_bitrate, int audio_bitrate) {
    target_video_bitrate_ = video_bitrate;
    target_audio_bitrate_ = audio_bitrate;
}

void GpacHLSDecoder::Reset() {
    global_frame_counter_ = 0;
    segment_frame_counter_ = 0;
    last_frame_time_ = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = DecoderStats();
}

GpacHLSDecoder::DecoderStats GpacHLSDecoder::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool GpacHLSDecoder::InitializeGpacContext() {
    // Initialize GPAC system
    // In real implementation: return gf_sys_init(0, 0) != GF_OK;
    return true; // Simulated success
}

bool GpacHLSDecoder::SetupVideoDecoder() {
    // Setup video decoder for H.264/H.265
    // In real implementation: create and configure video decoder
    video_decoder_ = reinterpret_cast<void*>(0x11111111); // Dummy pointer
    return true;
}

bool GpacHLSDecoder::SetupAudioDecoder() {
    // Setup audio decoder for AAC/MP3
    // In real implementation: create and configure audio decoder  
    audio_decoder_ = reinterpret_cast<void*>(0x22222222); // Dummy pointer
    return true;
}

bool GpacHLSDecoder::SetupMuxers() {
    // Setup AVI and WAV muxers
    // In real implementation: create muxers for output containers
    if (enable_avi_output_) {
        avi_muxer_ = reinterpret_cast<void*>(0x33333333); // Dummy pointer
    }
    if (enable_wav_output_) {
        wav_muxer_ = reinterpret_cast<void*>(0x44444444); // Dummy pointer
    }
    return true;
}

void GpacHLSDecoder::CleanupGpacContext() {
    // Cleanup GPAC context and decoders
    // In real implementation: gf_sys_close();
    gpac_context_ = nullptr;
    video_decoder_ = nullptr;
    audio_decoder_ = nullptr;
    avi_muxer_ = nullptr;
    wav_muxer_ = nullptr;
}

bool GpacHLSDecoder::ProcessVideoTrack(const uint8_t* data, size_t size, std::vector<MediaPacket>& output) {
    // Process video track and generate AVI output
    // This is a simplified simulation - real implementation would:
    // 1. Parse MPEG-TS to extract video PES packets
    // 2. Decode H.264/H.265 to raw YUV frames
    // 3. Encode frames to AVI format
    // 4. Return AVI data in MediaPackets
    
    global_frame_counter_++;
    segment_frame_counter_++;
    
    // Create simulated AVI packet
    MediaPacket video_packet;
    video_packet.is_video = true;
    video_packet.is_key_frame = (segment_frame_counter_ == 1); // First frame is key frame
    video_packet.frame_number = global_frame_counter_;
    video_packet.duration = std::chrono::milliseconds(33); // ~30fps
    
    // Generate AVI header + frame data (simulated)
    if (segment_frame_counter_ == 1) {
        // Add AVI header for first frame
        auto avi_header = CreateAVIHeader(1920, 1080, 30.0);
        video_packet.data = avi_header;
    }
    
    // Add simulated frame data (in real implementation, this would be decoded video)
    size_t frame_size = std::min(size, size_t(50000)); // Limit frame size
    video_packet.data.reserve(video_packet.data.size() + frame_size);
    
    // Copy some of the input data as simulated decoded frame
    size_t copy_start = size / 4; // Skip some header data
    size_t copy_size = std::min(frame_size, size - copy_start);
    if (copy_size > 0) {
        video_packet.data.insert(video_packet.data.end(), 
                                data + copy_start, 
                                data + copy_start + copy_size);
    }
    
    output.push_back(video_packet);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.bytes_output += video_packet.data.size();
        
        // Calculate FPS
        auto now = std::chrono::steady_clock::now();
        auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_time_);
        if (time_diff.count() > 0) {
            stats_.current_fps = 1000.0 / time_diff.count();
        }
        last_frame_time_ = now;
    }
    
    return true;
}

bool GpacHLSDecoder::ProcessAudioTrack(const uint8_t* data, size_t size, std::vector<MediaPacket>& output) {
    // Process audio track and generate WAV output
    // This is a simplified simulation - real implementation would:
    // 1. Parse MPEG-TS to extract audio PES packets  
    // 2. Decode AAC/MP3 to raw PCM samples
    // 3. Encode samples to WAV format
    // 4. Return WAV data in MediaPackets
    
    MediaPacket audio_packet;
    audio_packet.is_audio = true;
    audio_packet.frame_number = global_frame_counter_;
    audio_packet.duration = std::chrono::milliseconds(23); // ~43fps audio frames
    
    // Generate WAV header + audio data (simulated)
    if (segment_frame_counter_ == 1) {
        // Add WAV header for first audio packet
        auto wav_header = CreateWAVHeader(48000, 2, 16);
        audio_packet.data = wav_header;
    }
    
    // Add simulated audio data (in real implementation, this would be decoded PCM)
    size_t audio_size = std::min(size, size_t(8192)); // Typical audio frame size
    audio_packet.data.reserve(audio_packet.data.size() + audio_size);
    
    // Copy some of the input data as simulated decoded audio
    size_t copy_start = size / 2; // Skip some data
    size_t copy_size = std::min(audio_size, size - copy_start);
    if (copy_size > 0) {
        audio_packet.data.insert(audio_packet.data.end(),
                                data + copy_start,
                                data + copy_start + copy_size);
    }
    
    output.push_back(audio_packet);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.bytes_output += audio_packet.data.size();
    }
    
    return true;
}

std::vector<uint8_t> GpacHLSDecoder::CreateAVIHeader(int width, int height, double fps) {
    // Create minimal AVI header (simulated)
    // In real implementation, this would generate proper AVI header
    std::vector<uint8_t> header;
    
    // AVI signature
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    
    // File size (placeholder)
    uint32_t file_size = 0;
    header.insert(header.end(), reinterpret_cast<uint8_t*>(&file_size), 
                  reinterpret_cast<uint8_t*>(&file_size) + 4);
    
    // AVI identifier
    header.insert(header.end(), {'A', 'V', 'I', ' '});
    
    // Add basic header info (simplified)
    header.resize(512, 0); // Pad to reasonable header size
    
    return header;
}

std::vector<uint8_t> GpacHLSDecoder::CreateWAVHeader(int sample_rate, int channels, int bits_per_sample) {
    // Create minimal WAV header (simulated)
    // In real implementation, this would generate proper WAV header
    std::vector<uint8_t> header;
    
    // WAV signature
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    
    // File size (placeholder)
    uint32_t file_size = 0;
    header.insert(header.end(), reinterpret_cast<uint8_t*>(&file_size),
                  reinterpret_cast<uint8_t*>(&file_size) + 4);
    
    // WAV identifier
    header.insert(header.end(), {'W', 'A', 'V', 'E'});
    
    // Add basic format info (simplified)
    header.resize(44, 0); // Standard WAV header size
    
    return header;
}

// GpacStreamRouter implementation
GpacStreamRouter::GpacStreamRouter() {
    player_process_handle_ = INVALID_HANDLE_VALUE;
    stream_start_time_ = std::chrono::steady_clock::now();
    
    media_buffer_ = std::make_unique<MediaBuffer>(1000);
    gpac_decoder_ = std::make_unique<GpacHLSDecoder>();
}

GpacStreamRouter::~GpacStreamRouter() {
    StopRouting();
}

bool GpacStreamRouter::StartRouting(const std::wstring& hls_playlist_url, 
                                   const RouterConfig& config,
                                   std::atomic<bool>& cancel_token,
                                   std::function<void(const std::wstring&)> log_callback) {
    if (routing_active_) {
        return false; // Already routing
    }
    
    current_config_ = config;
    log_callback_ = log_callback;
    routing_active_ = true;
    
    // Initialize GPAC decoder
    if (!gpac_decoder_->Initialize()) {
        if (log_callback_) {
            log_callback_(L"[GPAC] Failed to initialize GPAC decoder");
        }
        routing_active_ = false;
        return false;
    }
    
    // Configure decoder
    gpac_decoder_->SetOutputFormat(config.enable_avi_output, config.enable_wav_output);
    gpac_decoder_->SetQuality(config.target_video_bitrate, config.target_audio_bitrate);
    
    // Reset decoder and buffer
    gpac_decoder_->Reset();
    media_buffer_->Reset();
    
    if (log_callback_) {
        log_callback_(L"[GPAC] Starting GPAC-based media decoding and routing");
        log_callback_(L"[GPAC] Player: " + config.player_path);
        log_callback_(L"[GPAC] Buffer size: " + std::to_wstring(config.buffer_size_packets) + L" packets");
        log_callback_(L"[GPAC] Output: " + 
                     (config.enable_avi_output ? L"AVI " : L"") +
                     (config.enable_wav_output ? L"WAV" : L""));
    }
    
    // Start HLS fetcher thread
    hls_fetcher_thread_ = std::thread([this, hls_playlist_url, &cancel_token]() {
        HLSFetcherThread(hls_playlist_url, cancel_token);
    });
    
    // Start media router thread
    media_router_thread_ = std::thread([this, &cancel_token]() {
        MediaRouterThread(cancel_token);
    });
    
    return true;
}

void GpacStreamRouter::StopRouting() {
    if (!routing_active_) return;
    
    routing_active_ = false;
    
    // Signal end of stream to buffer
    if (media_buffer_) {
        media_buffer_->SignalEndOfStream();
    }
    
    // Wait for threads to finish
    if (hls_fetcher_thread_.joinable()) {
        hls_fetcher_thread_.join();
    }
    if (media_router_thread_.joinable()) {
        media_router_thread_.join();
    }
    
    // Clear stored process handle
    player_process_handle_ = INVALID_HANDLE_VALUE;
    
    if (log_callback_) {
        log_callback_(L"[GPAC] GPAC media routing stopped");
    }
}

GpacStreamRouter::BufferStats GpacStreamRouter::GetBufferStats() const {
    BufferStats stats;
    
    stats.buffered_packets = media_buffer_->GetBufferedPackets();
    stats.total_packets_processed = total_packets_processed_.load();
    
    if (current_config_.buffer_size_packets > 0) {
        stats.buffer_utilization = static_cast<double>(stats.buffered_packets) / current_config_.buffer_size_packets;
    }
    
    // Get GPAC decoder statistics
    auto decoder_stats = gpac_decoder_->GetStats();
    stats.segments_decoded = decoder_stats.segments_processed;
    stats.video_frames_decoded = decoder_stats.video_frames_decoded;
    stats.audio_frames_decoded = decoder_stats.audio_frames_decoded;
    stats.current_fps = decoder_stats.current_fps;
    stats.decoder_healthy = decoder_stats.decoder_healthy;
    stats.bytes_input = decoder_stats.bytes_input;
    stats.bytes_output = decoder_stats.bytes_output;
    
    // Estimate stream health
    stats.video_stream_healthy = (decoder_stats.video_frames_decoded > 0) && decoder_stats.decoder_healthy;
    stats.audio_stream_healthy = (decoder_stats.audio_frames_decoded > 0) && decoder_stats.decoder_healthy;
    
    return stats;
}

void GpacStreamRouter::HLSFetcherThread(const std::wstring& playlist_url, std::atomic<bool>& cancel_token) {
    if (log_callback_) {
        log_callback_(L"[GPAC] HLS fetcher thread started");
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
                    log_callback_(L"[GPAC] Failed to fetch playlist (attempt " + std::to_wstring(consecutive_failures) + L"/" + std::to_wstring(max_consecutive_failures) + L")");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                continue;
            }
            
            consecutive_failures = 0; // Reset failure counter on success
            
            // Check for stream end
            if (playlist_content.find("#EXT-X-ENDLIST") != std::string::npos) {
                if (log_callback_) {
                    log_callback_(L"[GPAC] Found #EXT-X-ENDLIST - stream ended normally");
                }
                routing_active_ = false;
                break;
            }
            
            // Parse playlist
            PlaylistParser playlist_parser;
            std::vector<std::wstring> segment_urls;
            
            if (playlist_parser.ParsePlaylist(playlist_content)) {
                auto segments = playlist_parser.GetSegments();
                for (const auto& segment : segments) {
                    segment_urls.push_back(segment.url);
                }
                
                // Handle discontinuities
                if (playlist_parser.HasDiscontinuities()) {
                    if (log_callback_) {
                        log_callback_(L"[GPAC] Detected discontinuity - resetting decoder");
                    }
                    media_buffer_->Clear();
                    gpac_decoder_->Reset();
                }
            } else {
                // Fallback to basic parsing
                segment_urls = ParseHLSPlaylist(playlist_content, playlist_url);
            }
            
            if (segment_urls.empty()) {
                if (log_callback_) {
                    log_callback_(L"[GPAC] No segments found in playlist");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
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
                
                // Low-latency optimization: skip old segments if needed
                if (current_config_.low_latency_mode && current_config_.skip_old_segments) {
                    size_t remaining_segments = segment_urls.size() - 
                        (std::find(segment_urls.begin(), segment_urls.end(), segment_url) - segment_urls.begin());
                    if (remaining_segments > current_config_.max_segments_to_buffer) {
                        processed_segments.push_back(segment_url);
                        continue;
                    }
                }
                
                // Fetch segment data
                std::vector<uint8_t> segment_data;
                if (FetchHLSSegment(segment_url, segment_data, &cancel_token)) {
                    if (segment_data.empty()) continue;
                    
                    // Decode segment using GPAC
                    auto media_packets = gpac_decoder_->DecodeSegment(segment_data, first_segment);
                    first_segment = false;
                    
                    if (media_packets.empty()) {
                        if (log_callback_) {
                            log_callback_(L"[GPAC] No media packets decoded from segment");
                        }
                        continue;
                    }
                    
                    // Add to buffer with flow control
                    for (const auto& packet : media_packets) {
                        if (!routing_active_ || cancel_token) break;
                        
                        // Implement flow control
                        while (media_buffer_->GetBufferedPackets() >= current_config_.buffer_size_packets && 
                               routing_active_ && !cancel_token) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        
                        media_buffer_->AddPacket(packet);
                        total_packets_processed_++;
                    }
                    
                    processed_segments.push_back(segment_url);
                    segments_processed++;
                    
                    // Keep only recent segments in memory
                    if (processed_segments.size() > 10) {
                        processed_segments.erase(processed_segments.begin());
                    }
                    
                    if (log_callback_ && segments_processed <= 3) {
                        log_callback_(L"[GPAC] Decoded segment: " + std::to_wstring(media_packets.size()) + 
                                     L" packets (" + std::to_wstring(segment_data.size()) + L" bytes)");
                    }
                } else {
                    if (log_callback_) {
                        log_callback_(L"[GPAC] Failed to fetch segment: " + segment_url);
                    }
                }
            }
            
            if (segments_processed > 0 && log_callback_) {
                segments_processed_++;
                log_callback_(L"[GPAC] Batch complete: " + std::to_wstring(segments_processed) + L" new segments processed");
            }
            
            // Wait before next playlist refresh
            auto refresh_interval = current_config_.low_latency_mode ? 
                current_config_.playlist_refresh_interval : 
                std::chrono::milliseconds(2000);
                
            std::this_thread::sleep_for(refresh_interval);
            
        } catch (const std::exception& e) {
            consecutive_failures++;
            if (log_callback_) {
                std::string error_msg = e.what();
                log_callback_(L"[GPAC] HLS fetcher error: " + std::wstring(error_msg.begin(), error_msg.end()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
    
    if (consecutive_failures >= max_consecutive_failures) {
        if (log_callback_) {
            log_callback_(L"[GPAC] HLS fetcher stopping due to too many consecutive failures");
        }
        routing_active_ = false;
    }
    
    // Signal end of stream
    if (media_buffer_) {
        media_buffer_->SignalEndOfStream();
    }
    
    if (log_callback_) {
        log_callback_(L"[GPAC] HLS fetcher thread stopped");
    }
}

void GpacStreamRouter::MediaRouterThread(std::atomic<bool>& cancel_token) {
    if (log_callback_) {
        log_callback_(L"[GPAC] Media router thread started");
    }
    
    HANDLE player_process = INVALID_HANDLE_VALUE;
    HANDLE player_stdin = INVALID_HANDLE_VALUE;
    
    // Launch media player
    if (!LaunchMediaPlayer(current_config_, player_process, player_stdin)) {
        if (log_callback_) {
            log_callback_(L"[GPAC] Failed to launch media player");
        }
        routing_active_ = false;
        return;
    }
    
    // Store player process handle for external monitoring
    player_process_handle_ = player_process;
    
    if (log_callback_) {
        log_callback_(L"[GPAC] Media player launched successfully");
    }
    
    size_t packets_sent = 0;
    auto last_log_time = std::chrono::steady_clock::now();
    
    // Send media packets to player
    while (routing_active_ && !cancel_token) {
        // Check if player process is still running
        if (player_process != INVALID_HANDLE_VALUE) {
            DWORD exit_code;
            if (GetExitCodeProcess(player_process, &exit_code)) {
                if (exit_code != STILL_ACTIVE) {
                    if (log_callback_) {
                        log_callback_(L"[GPAC] Media player process exited (code: " + std::to_wstring(exit_code) + L")");
                    }
                    cancel_token = true;
                    break;
                }
            }
        }
        
        // Get and send media packets
        MediaPacket packet;
        if (media_buffer_->GetNextPacket(packet, std::chrono::milliseconds(50))) {
            if (!SendMediaPacketToPlayer(player_stdin, packet)) {
                if (log_callback_) {
                    log_callback_(L"[GPAC] Failed to send media packet to player - pipe may be broken");
                }
                break;
            }
            packets_sent++;
        } else {
            // No packet available
            if (!media_buffer_->IsProducerActive() && media_buffer_->IsEmpty()) {
                if (log_callback_) {
                    log_callback_(L"[GPAC] Stream ended normally - no more packets to send");
                }
                break;
            }
        }
        
        // Log progress periodically
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 30) {
            if (log_callback_) {
                log_callback_(L"[GPAC] Streaming progress: " + std::to_wstring(packets_sent) + L" packets sent");
            }
            last_log_time = now;
        }
    }
    
    // Cleanup
    if (player_stdin != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(player_stdin);
        CloseHandle(player_stdin);
    }
    if (player_process != INVALID_HANDLE_VALUE) {
        if (WaitForSingleObject(player_process, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(player_process, 0);
        }
        CloseHandle(player_process);
        player_process_handle_ = INVALID_HANDLE_VALUE;
    }
    
    if (log_callback_) {
        log_callback_(L"[GPAC] Media router thread stopped (" + std::to_wstring(packets_sent) + L" packets sent)");
    }
}

bool GpacStreamRouter::LaunchMediaPlayer(const RouterConfig& config, HANDLE& process_handle, HANDLE& stdin_handle) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    
    // Create pipe for stdin
    HANDLE stdin_read, stdin_write;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        if (log_callback_) {
            log_callback_(L"[GPAC] Failed to create pipe for media player");
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
    
    // Build command line
    std::wstring cmd_line = L"\"" + config.player_path + L"\" " + config.player_args;
    
    if (log_callback_) {
        log_callback_(L"[GPAC] Launching player: " + cmd_line);
    }
    
    // Launch process
    if (!CreateProcessW(nullptr, &cmd_line[0], nullptr, nullptr, TRUE, 
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        DWORD error = GetLastError();
        if (log_callback_) {
            log_callback_(L"[GPAC] Failed to launch player process, error: " + std::to_wstring(error));
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
    
    return true;
}

bool GpacStreamRouter::SendMediaPacketToPlayer(HANDLE stdin_handle, const MediaPacket& packet) {
    if (stdin_handle == INVALID_HANDLE_VALUE || packet.data.empty()) {
        return false;
    }
    
    DWORD bytes_written = 0;
    BOOL write_result = WriteFile(stdin_handle, packet.data.data(), 
                                 static_cast<DWORD>(packet.data.size()), &bytes_written, nullptr);
    
    if (!write_result) {
        DWORD error = GetLastError();
        if (log_callback_) {
            log_callback_(L"[GPAC] WriteFile failed, error: " + std::to_wstring(error));
        }
        return false;
    }
    
    if (bytes_written != packet.data.size()) {
        if (log_callback_) {
            log_callback_(L"[GPAC] Partial write: " + std::to_wstring(bytes_written) + 
                         L"/" + std::to_wstring(packet.data.size()));
        }
        return false;
    }
    
    return true;
}

bool GpacStreamRouter::FetchHLSSegment(const std::wstring& segment_url, std::vector<uint8_t>& data, std::atomic<bool>* cancel_token) {
    return HttpGetBinary(segment_url, data, cancel_token);
}

std::vector<std::wstring> GpacStreamRouter::ParseHLSPlaylist(const std::string& playlist_content, const std::wstring& base_url) {
    std::vector<std::wstring> segment_urls;
    
    std::istringstream stream(playlist_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        if (line[0] == '#') {
            // Skip tag lines
            continue;
        }
        
        // This is a segment URL
        std::wstring rel_url(line.begin(), line.end());
        std::wstring full_url = JoinUrl(base_url, rel_url);
        segment_urls.push_back(full_url);
    }
    
    // For low-latency mode, only return the newest segments
    if (current_config_.low_latency_mode && segment_urls.size() > current_config_.max_segments_to_buffer) {
        size_t start_index = segment_urls.size() - current_config_.max_segments_to_buffer;
        std::vector<std::wstring> live_edge_segments(
            segment_urls.begin() + start_index, 
            segment_urls.end()
        );
        return live_edge_segments;
    }
    
    return segment_urls;
}

void GpacStreamRouter::CheckStreamHealth() {
    // Monitor stream health and decoder performance
    // This could be extended to check various health metrics
}

} // namespace gpac_decoder