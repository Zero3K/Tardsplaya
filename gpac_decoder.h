#pragma once
// GPAC-based media decoder for Tardsplaya
// Replaces TSDuck functionality to decode audio and video into raw MP4/WAV
// which are then piped to the media player

#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <functional>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
typedef int HANDLE;
#define INVALID_HANDLE_VALUE -1
#endif

namespace gpac_decoder {

    // Raw media packet containing decoded audio/video data
    struct MediaPacket {
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point timestamp;
        bool is_video = false;
        bool is_audio = false;
        bool is_key_frame = false;
        uint64_t frame_number = 0;
        std::chrono::milliseconds duration{0};
        
        MediaPacket() {
            timestamp = std::chrono::steady_clock::now();
        }
        
        bool IsValid() const { return !data.empty(); }
    };
    
    // HLS segment with enhanced metadata
    struct HLSSegment {
        std::wstring url;
        std::chrono::milliseconds duration{0};
        double sequence_number = 0;
        bool has_discontinuity = false;
        
        HLSSegment() = default;
        HLSSegment(const std::wstring& segment_url, std::chrono::milliseconds dur = std::chrono::milliseconds(0))
            : url(segment_url), duration(dur) {}
    };
    
    // Enhanced playlist parser for GPAC processing
    class PlaylistParser {
    public:
        PlaylistParser();
        
        // Parse M3U8 playlist
        bool ParsePlaylist(const std::string& m3u8_content);
        
        // Get segments for processing
        std::vector<HLSSegment> GetSegments() const { return segments_; }
        
        // Get target duration
        std::chrono::milliseconds GetTargetDuration() const { return target_duration_; }
        
        // Check if playlist indicates live stream
        bool IsLiveStream() const { return is_live_; }
        
        // Get media sequence number
        int64_t GetMediaSequence() const { return media_sequence_; }
        
        // Calculate optimal buffer size
        int GetOptimalBufferSegments() const;
        
        // Get timing information
        std::chrono::milliseconds GetPlaylistDuration() const;
        
        // Check for discontinuities
        bool HasDiscontinuities() const { return has_discontinuities_; }
        
    private:
        std::vector<HLSSegment> segments_;
        std::chrono::milliseconds target_duration_{0};
        bool is_live_ = false;
        int64_t media_sequence_ = 0;
        bool has_discontinuities_ = false;
        
        // Parsing helper methods
        void ParseInfoLine(const std::string& line, HLSSegment& current_segment);
        double ExtractFloatFromTag(const std::string& line, const std::string& tag);
        void CalculatePreciseTiming();
    };
    
    // Media buffer for smooth playback
    class MediaBuffer {
    public:
        MediaBuffer(size_t max_packets = 1000);
        
        // Add media packet to buffer
        bool AddPacket(const MediaPacket& packet);
        
        // Get next packet from buffer
        bool GetNextPacket(MediaPacket& packet, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
        
        // Get buffer status
        size_t GetBufferedPackets() const;
        bool IsEmpty() const;
        bool IsFull() const;
        
        // Buffer management
        void Clear();
        void Reset();
        void SignalEndOfStream();
        bool IsProducerActive() const { return producer_active_.load(); }
        
    private:
        mutable std::mutex mutex_;
        std::queue<MediaPacket> packet_queue_;
        size_t max_packets_;
        std::atomic<bool> producer_active_{true};
    };
    
    // GPAC-based HLS decoder
    class GpacHLSDecoder {
    public:
        GpacHLSDecoder();
        ~GpacHLSDecoder();
        
        // Initialize GPAC decoder
        bool Initialize();
        
        // Decode HLS segment to raw AVI/WAV data
        std::vector<MediaPacket> DecodeSegment(const std::vector<uint8_t>& hls_data, bool is_first_segment = false);
        
        // Set decoder parameters
        void SetOutputFormat(bool enable_avi = true, bool enable_wav = true);
        void SetQuality(int video_bitrate = 0, int audio_bitrate = 0); // 0 = auto
        
        // Reset decoder state
        void Reset();
        
        // Get decoder statistics
        struct DecoderStats {
            uint64_t segments_processed = 0;
            uint64_t video_frames_decoded = 0;
            uint64_t audio_frames_decoded = 0;
            uint64_t bytes_input = 0;
            uint64_t bytes_output = 0;
            double current_fps = 0.0;
            bool decoder_healthy = true;
        };
        DecoderStats GetStats() const;
        
    private:
        // GPAC context and decoder state
        void* gpac_context_ = nullptr;
        void* video_decoder_ = nullptr;
        void* audio_decoder_ = nullptr;
        void* avi_muxer_ = nullptr;
        void* wav_muxer_ = nullptr;
        
        // Decoder configuration
        bool enable_avi_output_ = true;
        bool enable_wav_output_ = true;
        int target_video_bitrate_ = 0;
        int target_audio_bitrate_ = 0;
        
        // Frame tracking
        uint64_t global_frame_counter_ = 0;
        uint32_t segment_frame_counter_ = 0;
        std::chrono::steady_clock::time_point last_frame_time_;
        
        // Statistics
        mutable std::mutex stats_mutex_;
        DecoderStats stats_;
        
        // Internal decoder methods
        bool InitializeGpacContext();
        bool SetupVideoDecoder();
        bool SetupAudioDecoder();
        bool SetupMuxers();
        void CleanupGpacContext();
        
        // Segment processing
        bool ProcessVideoTrack(const uint8_t* data, size_t size, std::vector<MediaPacket>& output);
        bool ProcessAudioTrack(const uint8_t* data, size_t size, std::vector<MediaPacket>& output);
        
        // Output formatting
        std::vector<uint8_t> CreateAVIHeader(int width, int height, double fps);
        std::vector<uint8_t> CreateWAVHeader(int sample_rate, int channels, int bits_per_sample);
    };
    
    // GPAC-based stream router - replaces TSDuck transport stream router
    class GpacStreamRouter {
    public:
        GpacStreamRouter();
        ~GpacStreamRouter();
        
        // Router configuration
        struct RouterConfig {
            std::wstring player_path = L"mpv.exe";
            std::wstring player_args = L"-";  // Read from stdin
            size_t buffer_size_packets = 1000;
            bool enable_avi_output = true;
            bool enable_wav_output = true;
            int target_video_bitrate = 0;  // 0 = auto
            int target_audio_bitrate = 0;  // 0 = auto
            
            // Low-latency streaming optimizations
            bool low_latency_mode = true;
            size_t max_segments_to_buffer = 2;
            std::chrono::milliseconds playlist_refresh_interval{500};
            bool skip_old_segments = true;
        };
        
        // Start GPAC decoding and routing to media player
        bool StartRouting(const std::wstring& hls_playlist_url, 
                         const RouterConfig& config,
                         std::atomic<bool>& cancel_token,
                         std::function<void(const std::wstring&)> log_callback = nullptr);
        
        // Stop routing
        void StopRouting();
        
        // Get routing status
        bool IsRouting() const { return routing_active_; }
        
        // Get buffer and decoder statistics
        struct BufferStats {
            size_t buffered_packets = 0;
            size_t total_packets_processed = 0;
            double buffer_utilization = 0.0; // 0.0 to 1.0
            
            // GPAC decoder statistics
            uint64_t segments_decoded = 0;
            uint64_t video_frames_decoded = 0;
            uint64_t audio_frames_decoded = 0;
            double current_fps = 0.0;
            bool decoder_healthy = true;
            
            // Media stream health
            bool video_stream_healthy = true;
            bool audio_stream_healthy = true;
            uint64_t bytes_input = 0;
            uint64_t bytes_output = 0;
        };
        BufferStats GetBufferStats() const;
        
        // Get player process handle for external monitoring
        HANDLE GetPlayerProcessHandle() const { return player_process_handle_; }
        
    private:
        std::atomic<bool> routing_active_{false};
        std::thread gpac_streaming_thread_;
        std::atomic<size_t> total_bytes_streamed_{0};
        
        RouterConfig current_config_;
        std::function<void(const std::wstring&)> log_callback_;
        HANDLE player_process_handle_;
        
        // Statistics tracking
        mutable std::mutex stats_mutex_;
        std::chrono::steady_clock::time_point stream_start_time_;
        
        // Worker threads
        void GpacStreamingThread(const std::wstring& hls_url, std::atomic<bool>& cancel_token);
    };
    
} // namespace gpac_decoder