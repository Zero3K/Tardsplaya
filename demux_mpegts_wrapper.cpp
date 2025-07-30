#include "demux_mpegts_wrapper.h"

// Include demux-mpegts headers
#include "demux-mpegts/tsDemuxer.h"
#include "demux-mpegts/elementaryStream.h"
#include "demux-mpegts/debug.h"

// Include Tardsplaya headers
#include "twitch_api.h"
#include "playlist_parser.h"

#include <algorithm>
#include <sstream>
#include <regex>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// External functions from Tardsplaya
extern bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token = nullptr);
extern bool HttpGetBinary(const std::wstring& url, std::vector<uint8_t>& out, std::atomic<bool>* cancel_token = nullptr);
extern std::wstring Utf8ToWide(const std::string& str);
extern void AddDebugLog(const std::wstring& msg);

namespace tardsplaya {

namespace {
    // Helper function to convert TSDemux stream type to our StreamType
    StreamType ConvertStreamType(const TSDemux::ElementaryStream* es) {
        if (!es) return StreamType::UNKNOWN;
        
        switch (es->stream_type) {
            case TSDemux::STREAM_TYPE_VIDEO_MPEG1:
            case TSDemux::STREAM_TYPE_VIDEO_MPEG2:
            case TSDemux::STREAM_TYPE_VIDEO_H264:
            case TSDemux::STREAM_TYPE_VIDEO_HEVC:
            case TSDemux::STREAM_TYPE_VIDEO_MPEG4:
            case TSDemux::STREAM_TYPE_VIDEO_VC1:
                return StreamType::VIDEO;
                
            case TSDemux::STREAM_TYPE_AUDIO_MPEG1:
            case TSDemux::STREAM_TYPE_AUDIO_MPEG2:
            case TSDemux::STREAM_TYPE_AUDIO_AAC:
            case TSDemux::STREAM_TYPE_AUDIO_AAC_ADTS:
            case TSDemux::STREAM_TYPE_AUDIO_AAC_LATM:
            case TSDemux::STREAM_TYPE_AUDIO_AC3:
            case TSDemux::STREAM_TYPE_AUDIO_EAC3:
            case TSDemux::STREAM_TYPE_AUDIO_LPCM:
            case TSDemux::STREAM_TYPE_AUDIO_DTS:
                return StreamType::AUDIO;
                
            case TSDemux::STREAM_TYPE_DVB_SUBTITLE:
            case TSDemux::STREAM_TYPE_DVB_TELETEXT:
                return StreamType::SUBTITLE;
                
            default:
                return StreamType::UNKNOWN;
        }
    }

    // Custom debug callback for demux-mpegts
    void DemuxDebugCallback(int level, char* msg) {
        if (!msg) return;
        
        std::wstring wmsg = Utf8ToWide(std::string(msg));
        std::wstring levelStr;
        
        switch (level) {
            case DEMUX_DBG_ERROR: levelStr = L"[DEMUX-ERROR] "; break;
            case DEMUX_DBG_WARN:  levelStr = L"[DEMUX-WARN] "; break;
            case DEMUX_DBG_INFO:  levelStr = L"[DEMUX-INFO] "; break;
            case DEMUX_DBG_DEBUG: levelStr = L"[DEMUX-DEBUG] "; break;
            default: levelStr = L"[DEMUX] "; break;
        }
        
        AddDebugLog(levelStr + wmsg);
    }
}

DemuxMpegtsWrapper::DemuxMpegtsWrapper(const DemuxConfig& config)
    : config_(config)
    , demux_start_time_(std::chrono::steady_clock::now()) {
    
    // Initialize demux-mpegts debug system
    if (config_.enable_debug_logging) {
        TSDemux::DBGLevel(DEMUX_DBG_DEBUG);
        TSDemux::SetDBGMsgCallback(DemuxDebugCallback);
    } else {
        TSDemux::DBGLevel(DEMUX_DBG_ERROR); // Only show errors
        TSDemux::SetDBGMsgCallback(DemuxDebugCallback);
    }
    
    LogMessage(L"DemuxMpegtsWrapper initialized with separate streams: " + 
               std::wstring(config_.enable_separate_streams ? L"enabled" : L"disabled"));
}

DemuxMpegtsWrapper::~DemuxMpegtsWrapper() {
    StopDemuxing();
    LogMessage(L"DemuxMpegtsWrapper destroyed");
}

const unsigned char* DemuxMpegtsWrapper::ReadAV(uint64_t pos, size_t len) {
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    
    // Check if we have enough data in buffer
    if (!IsInputBufferReady(pos, len)) {
        return nullptr;
    }
    
    // Calculate buffer offset
    if (pos < buffer_position_ || pos >= buffer_position_ + input_buffer_.size()) {
        return nullptr; // Position out of range
    }
    
    size_t offset = static_cast<size_t>(pos - buffer_position_);
    if (offset + len > input_buffer_.size()) {
        return nullptr; // Not enough data
    }
    
    buffer_read_position_ = pos + len;
    return &input_buffer_[offset];
}

bool DemuxMpegtsWrapper::StartDemuxing(const std::wstring& hls_playlist_url,
                                      std::atomic<bool>& cancel_token,
                                      std::function<void(const std::wstring&)> log_callback) {
    if (demuxing_active_.load()) {
        LogError(L"Demuxing already active");
        return false;
    }
    
    log_callback_ = log_callback;
    LogMessage(L"Starting MPEG-TS demuxing for: " + hls_playlist_url);
    
    try {
        // Create AV context for demuxing
        av_context_ = std::make_unique<TSDemux::AVContext>(this, 0, config_.target_channel);
        
        // Reset state
        {
            std::lock_guard<std::mutex> lock(input_buffer_mutex_);
            input_buffer_.clear();
            buffer_position_ = 0;
            buffer_read_position_ = 0;
        }
        
        // Clear queues
        {
            std::lock_guard<std::mutex> lock(video_queue_mutex_);
            std::queue<DemuxedPacket> empty;
            video_packet_queue_.swap(empty);
        }
        {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            std::queue<DemuxedPacket> empty;
            audio_packet_queue_.swap(empty);
        }
        
        // Launch player processes if separate streams enabled
        if (config_.enable_separate_streams) {
            if (!LaunchVideoPlayer() || !LaunchAudioPlayer()) {
                LogError(L"Failed to launch media players");
                return false;
            }
        }
        
        demuxing_active_.store(true);
        demux_start_time_ = std::chrono::steady_clock::now();
        
        // Start worker threads
        hls_downloader_thread_ = std::thread(&DemuxMpegtsWrapper::HLSDownloaderThread, this, 
                                           hls_playlist_url, std::ref(cancel_token));
        demux_processor_thread_ = std::thread(&DemuxMpegtsWrapper::DemuxProcessorThread, this, 
                                            std::ref(cancel_token));
        
        if (config_.enable_separate_streams) {
            video_output_thread_ = std::thread(&DemuxMpegtsWrapper::VideoOutputThread, this, 
                                             std::ref(cancel_token));
            audio_output_thread_ = std::thread(&DemuxMpegtsWrapper::AudioOutputThread, this, 
                                             std::ref(cancel_token));
        }
        
        LogMessage(L"MPEG-TS demuxing started successfully");
        return true;
        
    } catch (const std::exception& e) {
        std::string error_msg = e.what();
        LogError(L"Exception starting demuxing: " + Utf8ToWide(error_msg));
        demuxing_active_.store(false);
        return false;
    }
}

void DemuxMpegtsWrapper::StopDemuxing() {
    if (!demuxing_active_.load()) {
        return;
    }
    
    LogMessage(L"Stopping MPEG-TS demuxing...");
    demuxing_active_.store(false);
    
    // Wait for threads to finish
    if (hls_downloader_thread_.joinable()) {
        hls_downloader_thread_.join();
    }
    if (demux_processor_thread_.joinable()) {
        demux_processor_thread_.join();
    }
    if (video_output_thread_.joinable()) {
        video_output_thread_.join();
    }
    if (audio_output_thread_.joinable()) {
        audio_output_thread_.join();
    }
    
    // Terminate player processes
    TerminateVideoPlayer();
    TerminateAudioPlayer();
    
    // Clear state
    av_context_.reset();
    
    LogMessage(L"MPEG-TS demuxing stopped");
}

std::vector<StreamInfo> DemuxMpegtsWrapper::GetAvailableStreams() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    std::vector<StreamInfo> streams;
    streams.reserve(available_streams_.size());
    
    for (const auto& pair : available_streams_) {
        streams.push_back(pair.second);
    }
    
    return streams;
}

bool DemuxMpegtsWrapper::EnableStream(uint16_t pid) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    if (available_streams_.find(pid) == available_streams_.end()) {
        LogError(L"Attempted to enable unknown stream PID: " + std::to_wstring(pid));
        return false;
    }
    
    enabled_streams_[pid] = true;
    
    if (av_context_) {
        av_context_->StartStreaming(pid);
    }
    
    LogMessage(L"Enabled stream PID: " + std::to_wstring(pid));
    return true;
}

bool DemuxMpegtsWrapper::DisableStream(uint16_t pid) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    enabled_streams_[pid] = false;
    
    if (av_context_) {
        av_context_->StopStreaming(pid);
    }
    
    LogMessage(L"Disabled stream PID: " + std::to_wstring(pid));
    return true;
}

void DemuxMpegtsWrapper::EnableStreamType(StreamType type) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    for (const auto& pair : available_streams_) {
        if (pair.second.type == type) {
            enabled_streams_[pair.first] = true;
            if (av_context_) {
                av_context_->StartStreaming(pair.first);
            }
        }
    }
    
    std::wstring type_name;
    switch (type) {
        case StreamType::VIDEO: type_name = L"VIDEO"; break;
        case StreamType::AUDIO: type_name = L"AUDIO"; break;
        case StreamType::SUBTITLE: type_name = L"SUBTITLE"; break;
        default: type_name = L"UNKNOWN"; break;
    }
    
    LogMessage(L"Enabled all streams of type: " + type_name);
}

void DemuxMpegtsWrapper::DisableStreamType(StreamType type) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    for (const auto& pair : available_streams_) {
        if (pair.second.type == type) {
            enabled_streams_[pair.first] = false;
            if (av_context_) {
                av_context_->StopStreaming(pair.first);
            }
        }
    }
    
    std::wstring type_name;
    switch (type) {
        case StreamType::VIDEO: type_name = L"VIDEO"; break;
        case StreamType::AUDIO: type_name = L"AUDIO"; break;
        case StreamType::SUBTITLE: type_name = L"SUBTITLE"; break;
        default: type_name = L"UNKNOWN"; break;
    }
    
    LogMessage(L"Disabled all streams of type: " + type_name);
}

DemuxMpegtsWrapper::DemuxStats DemuxMpegtsWrapper::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool DemuxMpegtsWrapper::IsVideoPlayerRunning() const {
    if (video_player_process_ == nullptr) return false;
    
    DWORD exit_code;
    if (GetExitCodeProcess(video_player_process_, &exit_code)) {
        return exit_code == STILL_ACTIVE;
    }
    return false;
}

bool DemuxMpegtsWrapper::IsAudioPlayerRunning() const {
    if (audio_player_process_ == nullptr) return false;
    
    DWORD exit_code;
    if (GetExitCodeProcess(audio_player_process_, &exit_code)) {
        return exit_code == STILL_ACTIVE;
    }
    return false;
}

void DemuxMpegtsWrapper::ResetStreams() {
    LogMessage(L"Resetting streams due to errors or discontinuities");
    
    // Reset AV context
    if (av_context_) {
        av_context_->Reset();
    }
    
    // Clear buffers
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        std::queue<DemuxedPacket> empty;
        video_packet_queue_.swap(empty);
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        std::queue<DemuxedPacket> empty;
        audio_packet_queue_.swap(empty);
    }
    
    // Reset stream health
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& pair : available_streams_) {
            pair.second.is_healthy = true;
            pair.second.error_count = 0;
        }
    }
    
    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.error_packets = 0;
        stats_.video_stream_healthy = true;
        stats_.audio_stream_healthy = true;
    }
}

void DemuxMpegtsWrapper::RecoverFromDiscontinuity() {
    LogMessage(L"Recovering from stream discontinuity");
    
    // This is the main feature - handle discontinuities gracefully
    // by resetting demux state and clearing buffers
    ResetStreams();
    
    // Restart players if needed
    if (config_.enable_separate_streams && config_.auto_restart_streams) {
        if (!IsVideoPlayerRunning()) {
            LogMessage(L"Restarting video player after discontinuity");
            TerminateVideoPlayer();
            LaunchVideoPlayer();
        }
        
        if (!IsAudioPlayerRunning()) {
            LogMessage(L"Restarting audio player after discontinuity");
            TerminateAudioPlayer();
            LaunchAudioPlayer();
        }
    }
}

bool DemuxMpegtsWrapper::HasStreamErrors() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_.error_packets > 0 || !stats_.video_stream_healthy || !stats_.audio_stream_healthy;
}

std::vector<std::wstring> DemuxMpegtsWrapper::GetLastErrors() const {
    std::lock_guard<std::mutex> lock(errors_mutex_);
    return recent_errors_;
}

// Thread functions implementation

void DemuxMpegtsWrapper::HLSDownloaderThread(const std::wstring& playlist_url, std::atomic<bool>& cancel_token) {
    LogMessage(L"HLS downloader thread started");
    
    std::vector<std::wstring> processed_segments;
    
    while (demuxing_active_.load() && !cancel_token.load()) {
        try {
            // Fetch playlist
            std::string playlist_content;
            if (!HttpGetText(playlist_url, playlist_content, &cancel_token)) {
                LogError(L"Failed to fetch HLS playlist");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            // Parse segments
            auto segments = ParseHLSPlaylist(playlist_content, playlist_url);
            
            // Download new segments
            for (const auto& segment_url : segments) {
                if (cancel_token.load() || !demuxing_active_.load()) break;
                
                // Skip already processed segments
                if (std::find(processed_segments.begin(), processed_segments.end(), segment_url) != processed_segments.end()) {
                    continue;
                }
                
                // Download segment
                std::vector<uint8_t> segment_data;
                if (FetchHLSSegment(segment_url, segment_data, &cancel_token)) {
                    // Add to input buffer
                    AppendToInputBuffer(segment_data);
                    processed_segments.push_back(segment_url);
                    
                    // Keep only recent segments in memory
                    if (processed_segments.size() > 10) {
                        processed_segments.erase(processed_segments.begin());
                    }
                    
                    LogMessage(L"Downloaded segment: " + segment_url.substr(segment_url.find_last_of(L'/') + 1));
                } else {
                    LogError(L"Failed to download segment: " + segment_url);
                }
            }
            
            // Clean old buffer data periodically
            ClearOldBufferData();
            
            // Wait before next playlist refresh
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            
        } catch (const std::exception& e) {
            LogError(L"Exception in HLS downloader: " + Utf8ToWide(e.what()));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    LogMessage(L"HLS downloader thread stopped");
}

void DemuxMpegtsWrapper::DemuxProcessorThread(std::atomic<bool>& cancel_token) {
    LogMessage(L"Demux processor thread started");
    
    while (demuxing_active_.load() && !cancel_token.load()) {
        try {
            if (!av_context_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            // Try to resync
            int ret = av_context_->TSResync();
            if (ret != TSDemux::AVCONTEXT_CONTINUE) {
                if (ret == TSDemux::AVCONTEXT_TS_NOSYNC) {
                    // No more data, wait for more
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (ret == TSDemux::AVCONTEXT_IO_ERROR) {
                    LogError(L"IO error in demux processor");
                    break;
                } else {
                    LogError(L"TS error in demux processor: " + std::to_wstring(ret));
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            }
            
            // Process TS packet
            ret = av_context_->ProcessTSPacket();
            
            // Handle stream data
            if (av_context_->HasPIDStreamData()) {
                TSDemux::STREAM_PKT pkt;
                while (av_context_->GetPIDStream() && av_context_->GetPIDStream()->GetStreamPacket(&pkt)) {
                    if (pkt.streamChange) {
                        // Update stream info
                        UpdateStreamInfo(pkt.pid, av_context_->GetPIDStream());
                    }
                    
                    // Process the packet
                    ProcessDemuxedPacket(pkt);
                }
            }
            
            // Handle payload processing
            if (av_context_->HasPIDPayload()) {
                ret = av_context_->ProcessTSPayload();
                if (ret == TSDemux::AVCONTEXT_PROGRAM_CHANGE) {
                    LogMessage(L"Program change detected, updating streams");
                    
                    // Get all streams and update our stream list
                    auto streams = av_context_->GetStreams();
                    for (auto* es : streams) {
                        if (es && es->has_stream_info) {
                            UpdateStreamInfo(es->pid, es);
                            // Auto-enable video and audio streams
                            if (ConvertStreamType(es) == StreamType::VIDEO || ConvertStreamType(es) == StreamType::AUDIO) {
                                EnableStream(es->pid);
                            }
                        }
                    }
                }
            }
            
            // Handle errors
            if (ret < 0) {
                if (ret == TSDemux::AVCONTEXT_TS_ERROR) {
                    av_context_->Shift();
                    {
                        std::lock_guard<std::mutex> lock(stats_mutex_);
                        stats_.error_packets++;
                    }
                } else if (ret == TSDemux::AVCONTEXT_DISCONTINUITY) {
                    LogMessage(L"Discontinuity detected - recovering");
                    RecoverFromDiscontinuity();
                }
            } else {
                av_context_->GoNext();
            }
            
            // Update statistics
            UpdateStats();
            
        } catch (const std::exception& e) {
            LogError(L"Exception in demux processor: " + Utf8ToWide(e.what()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LogMessage(L"Demux processor thread stopped");
}

void DemuxMpegtsWrapper::VideoOutputThread(std::atomic<bool>& cancel_token) {
    LogMessage(L"Video output thread started");
    
    while (demuxing_active_.load() && !cancel_token.load()) {
        try {
            DemuxedPacket packet;
            bool has_packet = false;
            
            // Get packet from video queue
            {
                std::lock_guard<std::mutex> lock(video_queue_mutex_);
                if (!video_packet_queue_.empty()) {
                    packet = video_packet_queue_.front();
                    video_packet_queue_.pop();
                    has_packet = true;
                }
            }
            
            if (has_packet && packet.type == StreamType::VIDEO) {
                if (!SendVideoPacket(packet)) {
                    LogError(L"Failed to send video packet to player");
                    // Try to restart video player
                    if (config_.auto_restart_streams) {
                        TerminateVideoPlayer();
                        if (!LaunchVideoPlayer()) {
                            LogError(L"Failed to restart video player");
                            break;
                        }
                    }
                }
            } else {
                // No packets available, wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
        } catch (const std::exception& e) {
            LogError(L"Exception in video output: " + Utf8ToWide(e.what()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LogMessage(L"Video output thread stopped");
}

void DemuxMpegtsWrapper::AudioOutputThread(std::atomic<bool>& cancel_token) {
    LogMessage(L"Audio output thread started");
    
    while (demuxing_active_.load() && !cancel_token.load()) {
        try {
            DemuxedPacket packet;
            bool has_packet = false;
            
            // Get packet from audio queue
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                if (!audio_packet_queue_.empty()) {
                    packet = audio_packet_queue_.front();
                    audio_packet_queue_.pop();
                    has_packet = true;
                }
            }
            
            if (has_packet && packet.type == StreamType::AUDIO) {
                if (!SendAudioPacket(packet)) {
                    LogError(L"Failed to send audio packet to player");
                    // Try to restart audio player
                    if (config_.auto_restart_streams) {
                        TerminateAudioPlayer();
                        if (!LaunchAudioPlayer()) {
                            LogError(L"Failed to restart audio player");
                            break;
                        }
                    }
                }
            } else {
                // No packets available, wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
        } catch (const std::exception& e) {
            LogError(L"Exception in audio output: " + Utf8ToWide(e.what()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LogMessage(L"Audio output thread stopped");
}

bool DemuxMpegtsWrapper::ProcessDemuxedPacket(const TSDemux::STREAM_PKT& pkt) {
    if (!pkt.data || pkt.size == 0) return false;
    
    // Create our packet
    DemuxedPacket demuxed;
    demuxed.pid = pkt.pid;
    demuxed.pts = pkt.pts;
    demuxed.dts = pkt.dts;
    demuxed.duration = pkt.duration;
    demuxed.stream_change = pkt.streamChange;
    demuxed.data.assign(pkt.data, pkt.data + pkt.size);
    
    // Determine stream type
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = available_streams_.find(pkt.pid);
        if (it != available_streams_.end()) {
            demuxed.type = it->second.type;
        } else {
            demuxed.type = StreamType::UNKNOWN;
        }
    }
    
    // Check if this stream is enabled
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = enabled_streams_.find(pkt.pid);
        if (it == enabled_streams_.end() || !it->second) {
            return true; // Stream disabled, but not an error
        }
    }
    
    // Add to appropriate queue
    bool queued = false;
    if (demuxed.type == StreamType::VIDEO) {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        if (video_packet_queue_.size() < config_.max_video_buffer_packets) {
            video_packet_queue_.push(demuxed);
            queued = true;
            
            // Update stats
            {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.video_packets_processed++;
                stats_.buffered_video_packets = video_packet_queue_.size();
            }
        }
    } else if (demuxed.type == StreamType::AUDIO) {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (audio_packet_queue_.size() < config_.max_audio_buffer_packets) {
            audio_packet_queue_.push(demuxed);
            queued = true;
            
            // Update stats
            {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.audio_packets_processed++;
                stats_.buffered_audio_packets = audio_packet_queue_.size();
            }
        }
    }
    
    if (!queued && (demuxed.type == StreamType::VIDEO || demuxed.type == StreamType::AUDIO)) {
        LogError(L"Packet queue full for PID " + std::to_wstring(pkt.pid));
        return false;
    }
    
    return true;
}

void DemuxMpegtsWrapper::UpdateStreamInfo(uint16_t pid, TSDemux::ElementaryStream* es) {
    if (!es) return;
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    StreamInfo& info = available_streams_[pid];
    info.pid = pid;
    info.type = ConvertStreamType(es);
    info.codec_name = es->GetStreamCodecName() ? es->GetStreamCodecName() : "unknown";
    
    // Copy stream info
    const auto& stream_info = es->stream_info;
    if (stream_info.language[0] != '\0') {
        info.language = std::string(stream_info.language, 4);
    }
    
    // Video info
    if (info.type == StreamType::VIDEO) {
        info.width = stream_info.width;
        info.height = stream_info.height;
        info.aspect_ratio = stream_info.aspect;
        info.fps_scale = stream_info.fps_scale;
        info.fps_rate = stream_info.fps_rate;
        info.interlaced = stream_info.interlaced;
    }
    
    // Audio info
    if (info.type == StreamType::AUDIO) {
        info.channels = stream_info.channels;
        info.sample_rate = stream_info.sample_rate;
        info.bit_rate = stream_info.bit_rate;
        info.bits_per_sample = stream_info.bits_per_sample;
    }
    
    info.last_packet_time = std::chrono::steady_clock::now();
    info.packet_count++;
    info.is_healthy = true;
    
    LogMessage(L"Updated stream info for PID " + std::to_wstring(pid) + 
               L" (" + Utf8ToWide(info.codec_name) + L")");
}

StreamType DemuxMpegtsWrapper::DetermineStreamType(const TSDemux::ElementaryStream* es) const {
    return ConvertStreamType(es);
}

// Player management functions

bool DemuxMpegtsWrapper::LaunchVideoPlayer() {
    if (IsVideoPlayerRunning()) {
        return true; // Already running
    }
    
    // Build command line for video-only player
    std::wstring cmdline = config_.player_path + L" " + config_.video_player_args;
    
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    
    // Create pipes for stdin
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hReadPipe, hWritePipe;
    
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        LogError(L"Failed to create pipe for video player");
        return false;
    }
    
    // Set up process to use our pipe for stdin
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hReadPipe;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    // Launch video player
    if (!CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmdline.c_str()),
        nullptr, nullptr, TRUE, 0, nullptr, nullptr,
        &si, &pi)) {
        LogError(L"Failed to launch video player: " + cmdline);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }
    
    // Store handles
    video_player_process_ = pi.hProcess;
    video_player_stdin_ = hWritePipe;
    
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    
    LogMessage(L"Video player launched successfully");
    return true;
}

bool DemuxMpegtsWrapper::LaunchAudioPlayer() {
    if (IsAudioPlayerRunning()) {
        return true; // Already running
    }
    
    // Build command line for audio-only player
    std::wstring cmdline = config_.player_path + L" " + config_.audio_player_args;
    
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    
    // Create pipes for stdin
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hReadPipe, hWritePipe;
    
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        LogError(L"Failed to create pipe for audio player");
        return false;
    }
    
    // Set up process to use our pipe for stdin
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hReadPipe;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    // Launch audio player
    if (!CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmdline.c_str()),
        nullptr, nullptr, TRUE, 0, nullptr, nullptr,
        &si, &pi)) {
        LogError(L"Failed to launch audio player: " + cmdline);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }
    
    // Store handles
    audio_player_process_ = pi.hProcess;
    audio_player_stdin_ = hWritePipe;
    
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    
    LogMessage(L"Audio player launched successfully");
    return true;
}

void DemuxMpegtsWrapper::TerminateVideoPlayer() {
    if (video_player_stdin_) {
        CloseHandle(video_player_stdin_);
        video_player_stdin_ = nullptr;
    }
    
    if (video_player_process_) {
        // Try to terminate gracefully first
        if (TerminateProcess(video_player_process_, 0)) {
            WaitForSingleObject(video_player_process_, 3000); // Wait up to 3 seconds
        }
        CloseHandle(video_player_process_);
        video_player_process_ = nullptr;
    }
    
    LogMessage(L"Video player terminated");
}

void DemuxMpegtsWrapper::TerminateAudioPlayer() {
    if (audio_player_stdin_) {
        CloseHandle(audio_player_stdin_);
        audio_player_stdin_ = nullptr;
    }
    
    if (audio_player_process_) {
        // Try to terminate gracefully first
        if (TerminateProcess(audio_player_process_, 0)) {
            WaitForSingleObject(audio_player_process_, 3000); // Wait up to 3 seconds
        }
        CloseHandle(audio_player_process_);
        audio_player_process_ = nullptr;
    }
    
    LogMessage(L"Audio player terminated");
}

bool DemuxMpegtsWrapper::SendVideoPacket(const DemuxedPacket& packet) {
    if (!video_player_stdin_ || packet.data.empty()) {
        return false;
    }
    
    DWORD written = 0;
    BOOL result = WriteFile(video_player_stdin_, packet.data.data(), 
                           static_cast<DWORD>(packet.data.size()), &written, nullptr);
    
    if (!result || written != packet.data.size()) {
        LogError(L"Failed to write video packet to player");
        return false;
    }
    
    return true;
}

bool DemuxMpegtsWrapper::SendAudioPacket(const DemuxedPacket& packet) {
    if (!audio_player_stdin_ || packet.data.empty()) {
        return false;
    }
    
    DWORD written = 0;
    BOOL result = WriteFile(audio_player_stdin_, packet.data.data(),
                           static_cast<DWORD>(packet.data.size()), &written, nullptr);
    
    if (!result || written != packet.data.size()) {
        LogError(L"Failed to write audio packet to player");
        return false;
    }
    
    return true;
}

// Utility functions

void DemuxMpegtsWrapper::LogMessage(const std::wstring& message) {
    if (log_callback_) {
        log_callback_(L"[DEMUX] " + message);
    }
    AddDebugLog(L"[DEMUX] " + message);
}

void DemuxMpegtsWrapper::LogError(const std::wstring& error) {
    if (log_callback_) {
        log_callback_(L"[DEMUX-ERROR] " + error);
    }
    AddDebugLog(L"[DEMUX-ERROR] " + error);
    
    // Store recent errors
    {
        std::lock_guard<std::mutex> lock(errors_mutex_);
        recent_errors_.push_back(error);
        
        // Keep only last 10 errors
        if (recent_errors_.size() > 10) {
            recent_errors_.erase(recent_errors_.begin());
        }
        
        last_error_time_ = std::chrono::steady_clock::now();
    }
}

void DemuxMpegtsWrapper::UpdateStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_packets_processed++;
    
    // Update buffered packet counts
    {
        std::lock_guard<std::mutex> video_lock(video_queue_mutex_);
        stats_.buffered_video_packets = video_packet_queue_.size();
    }
    {
        std::lock_guard<std::mutex> audio_lock(audio_queue_mutex_);
        stats_.buffered_audio_packets = audio_packet_queue_.size();
    }
    
    // Update stream health
    stats_.video_stream_healthy = IsVideoPlayerRunning();
    stats_.audio_stream_healthy = IsAudioPlayerRunning();
    
    // Calculate demux FPS
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - demux_start_time_);
    if (duration.count() > 0) {
        stats_.demux_fps = static_cast<double>(stats_.total_packets_processed) / duration.count();
    }
    
    stats_.last_update = now;
}

bool DemuxMpegtsWrapper::FetchHLSSegment(const std::wstring& segment_url, std::vector<uint8_t>& data, std::atomic<bool>* cancel_token) {
    return HttpGetBinary(segment_url, data, cancel_token);
}

std::vector<std::wstring> DemuxMpegtsWrapper::ParseHLSPlaylist(const std::string& playlist_content, const std::wstring& base_url) {
    std::vector<std::wstring> segments;
    std::istringstream iss(playlist_content);
    std::string line;
    
    // Extract base URL for relative paths
    std::wstring base_path = base_url;
    size_t last_slash = base_path.find_last_of(L'/');
    if (last_slash != std::wstring::npos) {
        base_path = base_path.substr(0, last_slash + 1);
    }
    
    while (std::getline(iss, line)) {
        // Skip empty lines and comments (except segment URLs)
        if (line.empty() || (line[0] == '#' && line.find("#EXT") != 0)) {
            continue;
        }
        
        // Look for segment URLs (lines not starting with #)
        if (line[0] != '#') {
            std::wstring segment_url = Utf8ToWide(line);
            
            // Handle relative URLs
            if (segment_url.find(L"http") != 0) {
                segment_url = base_path + segment_url;
            }
            
            segments.push_back(segment_url);
        }
    }
    
    return segments;
}

void DemuxMpegtsWrapper::AppendToInputBuffer(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    
    // Append data to buffer
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    
    LogMessage(L"Appended " + std::to_wstring(data.size()) + L" bytes to input buffer (total: " + 
               std::to_wstring(input_buffer_.size()) + L" bytes)");
}

bool DemuxMpegtsWrapper::IsInputBufferReady(uint64_t pos, size_t len) const {
    if (pos < buffer_position_) return false;
    if (pos >= buffer_position_ + input_buffer_.size()) return false;
    
    size_t offset = static_cast<size_t>(pos - buffer_position_);
    return (offset + len) <= input_buffer_.size();
}

void DemuxMpegtsWrapper::ClearOldBufferData() {
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    
    // Keep some buffer for back-reading, but don't let it grow too large
    const size_t max_buffer_size = 50 * 1024 * 1024; // 50MB max
    const size_t keep_size = 5 * 1024 * 1024;        // Keep 5MB of old data
    
    if (input_buffer_.size() > max_buffer_size && buffer_read_position_ > keep_size) {
        size_t remove_size = static_cast<size_t>(buffer_read_position_ - keep_size);
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + remove_size);
        buffer_position_ += remove_size;
        buffer_read_position_ = keep_size;
        
        LogMessage(L"Cleared " + std::to_wstring(remove_size) + L" bytes from input buffer");
    }
}

void DemuxMpegtsWrapper::CheckStreamHealth() {
    auto now = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& pair : available_streams_) {
        auto& stream = pair.second;
        
        // Check if stream has been inactive for too long
        auto inactive_time = std::chrono::duration_cast<std::chrono::milliseconds>(now - stream.last_packet_time);
        if (inactive_time > config_.stream_timeout) {
            if (stream.is_healthy) {
                stream.is_healthy = false;
                LogError(L"Stream PID " + std::to_wstring(stream.pid) + L" timed out");
            }
        }
        
        // Check error count
        if (stream.error_count > config_.max_consecutive_errors) {
            if (stream.is_healthy) {
                stream.is_healthy = false;
                LogError(L"Stream PID " + std::to_wstring(stream.pid) + L" has too many errors");
            }
        }
    }
}

void DemuxMpegtsWrapper::HandleStreamTimeout(uint16_t pid) {
    LogMessage(L"Handling timeout for stream PID " + std::to_wstring(pid));
    
    // Try to recover the stream
    if (av_context_) {
        av_context_->StopStreaming(pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        av_context_->StartStreaming(pid);
    }
}

void DemuxMpegtsWrapper::RestartFailedStreams() {
    if (!config_.auto_restart_streams) return;
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (const auto& pair : available_streams_) {
        const auto& stream = pair.second;
        
        if (!stream.is_healthy && enabled_streams_[stream.pid]) {
            LogMessage(L"Attempting to restart failed stream PID " + std::to_wstring(stream.pid));
            
            if (stream.type == StreamType::VIDEO && !IsVideoPlayerRunning()) {
                TerminateVideoPlayer();
                LaunchVideoPlayer();
            } else if (stream.type == StreamType::AUDIO && !IsAudioPlayerRunning()) {
                TerminateAudioPlayer();
                LaunchAudioPlayer();
            }
            
            HandleStreamTimeout(stream.pid);
        }
    }
}

// Factory function
std::unique_ptr<DemuxMpegtsWrapper> CreateDemuxWrapper(
    const std::wstring& player_path,
    bool enable_separate_streams,
    bool enable_debug_logging) {
    
    DemuxConfig config;
    config.player_path = player_path;
    config.enable_separate_streams = enable_separate_streams;
    config.enable_debug_logging = enable_debug_logging;
    
    return std::make_unique<DemuxMpegtsWrapper>(config);
}

} // namespace tardsplaya