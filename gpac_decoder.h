#pragma once
// GPAC-based media decoder for Tardsplaya
// Uses GPAC library directly for HLS processing and MP4 output
// No external gpac.exe dependencies

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

// GPAC library headers
extern "C" {
#include <gpac/tools.h>
#include <gpac/filters.h>
#include <gpac/isomedia.h>
#include <gpac/constants.h>
}

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
    
    // GPAC-based HLS decoder using libgpac
    class GpacHLSDecoder {
    public:
        GpacHLSDecoder();
        ~GpacHLSDecoder();
        
        // Initialize GPAC library and filter session
        bool Initialize();
        
        // Process HLS URL directly to MP4 output
        bool ProcessHLS(const std::wstring& hls_url, std::vector<uint8_t>& mp4_output, std::wstring& error_msg);
        
        // Set decoder parameters
        void SetOutputFormat(const std::string& format = "mp4"); // mp4, avi, wav
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
        // GPAC library context
        GF_FilterSession* filter_session_ = nullptr;
        GF_Filter* input_filter_ = nullptr;
        GF_Filter* output_filter_ = nullptr;
        
        // Output buffer for collecting MP4 data
        std::vector<uint8_t> output_buffer_;
        std::mutex output_mutex_;
        
        // Decoder configuration
        std::string output_format_ = "mp4";
        int target_video_bitrate_ = 0;
        int target_audio_bitrate_ = 0;
        
        // Statistics
        mutable std::mutex stats_mutex_;
        DecoderStats stats_;
        
        // Internal GPAC methods
        bool InitializeGpacLibrary();
        void CleanupGpacLibrary();
        bool CreateFilterSession();
        bool SetupHLSInput(const std::wstring& hls_url);
        bool SetupMP4Output();
        bool RunFilterSession();
        void CollectOutputData();
        
        // GPAC callbacks
        static void OnFilterOutput(void* user_data, const uint8_t* data, size_t size);
    };
    
    // GPAC-based stream router - direct library integration
    class GpacStreamRouter {
    public:
        GpacStreamRouter();
        ~GpacStreamRouter();
        
        // Router configuration
        struct RouterConfig {
            std::wstring player_path = L"mpv.exe";
            std::wstring player_args = L"-";  // Read from stdin
            int target_video_bitrate = 0;  // 0 = auto
            int target_audio_bitrate = 0;  // 0 = auto
            
            // Output format options
            bool use_mp4_output = true;  // Use MP4 instead of AVI/WAV
            std::string output_format = "mp4";  // mp4, avi, wav
        };
        
        // Start GPAC processing and routing to media player
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
        std::thread gpac_processing_thread_;
        std::atomic<size_t> total_bytes_processed_{0};
        
        RouterConfig current_config_;
        std::function<void(const std::wstring&)> log_callback_;
        HANDLE player_process_handle_;
        
        // GPAC decoder for direct library integration
        std::unique_ptr<GpacHLSDecoder> gpac_decoder_;
        
        // Statistics tracking
        mutable std::mutex stats_mutex_;
        std::chrono::steady_clock::time_point stream_start_time_;
        
        // Worker thread using GPAC library directly
        void GpacProcessingThread(const std::wstring& hls_url, std::atomic<bool>& cancel_token);
        bool LaunchMediaPlayer(const RouterConfig& config, HANDLE& process_handle, HANDLE& stdin_handle);
        bool SendDataToPlayer(HANDLE stdin_handle, const std::vector<uint8_t>& data);
    };
    
} // namespace gpac_decoder