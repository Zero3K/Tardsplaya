#pragma once
// Demux-MPEGTS Integration Wrapper for Tardsplaya
// Provides separate video and audio stream demuxing to prevent discontinuity issues

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <chrono>
#include <functional>
#include <memory>
#include <map>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Forward declare demux-mpegts classes to avoid including headers in .h file
namespace TSDemux {
    class AVContext;
    class TSDemuxer;
    struct STREAM_PKT;
    class ElementaryStream;
}

namespace tardsplaya {

    // Stream type identification for separate handling
    enum class StreamType {
        UNKNOWN = 0,
        VIDEO,
        AUDIO,
        SUBTITLE,
        DATA
    };

    // Elementary stream information
    struct StreamInfo {
        uint16_t pid = 0;
        StreamType type = StreamType::UNKNOWN;
        std::string codec_name;
        std::string language;
        
        // Video specific
        int width = 0;
        int height = 0;
        float aspect_ratio = 0.0f;
        int fps_scale = 0;
        int fps_rate = 0;
        bool interlaced = false;
        
        // Audio specific
        int channels = 0;
        int sample_rate = 0;
        int bit_rate = 0;
        int bits_per_sample = 0;
        
        // Stream health
        bool is_healthy = true;
        std::chrono::steady_clock::time_point last_packet_time;
        uint64_t packet_count = 0;
        uint64_t error_count = 0;
    };

    // Demuxed stream packet
    struct DemuxedPacket {
        uint16_t pid = 0;
        StreamType type = StreamType::UNKNOWN;
        std::vector<uint8_t> data;
        uint64_t pts = 0;  // Presentation timestamp
        uint64_t dts = 0;  // Decode timestamp
        uint64_t duration = 0;
        bool stream_change = false;
        std::chrono::steady_clock::time_point timestamp;
        
        DemuxedPacket() {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    // Configuration for demux-mpegts integration
    struct DemuxConfig {
        std::wstring player_path = L"mpv.exe";
        std::wstring video_player_args = L"--video-only --no-audio --"; // Video-only stream
        std::wstring audio_player_args = L"--audio-only --no-video --"; // Audio-only stream
        std::wstring combined_player_args = L"--"; // Combined stream (fallback)
        
        // Demuxing behavior
        bool enable_separate_streams = true;  // Main feature: separate video/audio
        bool enable_stream_recovery = true;   // Recover from discontinuities
        bool enable_packet_buffering = true;  // Buffer packets for smooth playback
        uint16_t target_channel = 0;          // Channel to demux (0 = all)
        
        // Buffer settings
        size_t max_video_buffer_packets = 1000;  // ~5-10 seconds of video
        size_t max_audio_buffer_packets = 2000;  // ~10-20 seconds of audio
        std::chrono::milliseconds buffer_timeout{5000}; // Timeout for buffering
        
        // Stream recovery settings
        std::chrono::milliseconds stream_timeout{10000}; // Consider stream dead after this
        uint32_t max_consecutive_errors = 10;  // Max errors before stream reset
        bool auto_restart_streams = true;      // Automatically restart failed streams
        
        // Logging
        bool enable_debug_logging = false;
        std::wstring log_file_path;  // Optional log file for demux debugging
    };

    // Main demux-mpegts wrapper class
    class DemuxMpegtsWrapper : public TSDemux::TSDemuxer {
    public:
        DemuxMpegtsWrapper(const DemuxConfig& config);
        virtual ~DemuxMpegtsWrapper();

        // TSDemuxer interface implementation
        virtual const unsigned char* ReadAV(uint64_t pos, size_t len) override;

        // Main streaming interface
        bool StartDemuxing(const std::wstring& hls_playlist_url,
                          std::atomic<bool>& cancel_token,
                          std::function<void(const std::wstring&)> log_callback = nullptr);
        void StopDemuxing();
        bool IsDemuxing() const { return demuxing_active_; }

        // Stream management
        std::vector<StreamInfo> GetAvailableStreams() const;
        bool EnableStream(uint16_t pid);
        bool DisableStream(uint16_t pid);
        void EnableStreamType(StreamType type);
        void DisableStreamType(StreamType type);

        // Stream health monitoring
        struct DemuxStats {
            size_t total_packets_processed = 0;
            size_t video_packets_processed = 0;
            size_t audio_packets_processed = 0;
            size_t subtitle_packets_processed = 0;
            size_t error_packets = 0;
            size_t buffered_video_packets = 0;
            size_t buffered_audio_packets = 0;
            
            // Stream health
            bool video_stream_healthy = false;
            bool audio_stream_healthy = false;
            std::chrono::milliseconds video_stream_lag{0};
            std::chrono::milliseconds audio_stream_lag{0};
            
            // Performance metrics
            double demux_fps = 0.0;
            std::chrono::steady_clock::time_point last_update;
        };
        DemuxStats GetStats() const;

        // Player process management
        HANDLE GetVideoPlayerProcess() const { return video_player_process_; }
        HANDLE GetAudioPlayerProcess() const { return audio_player_process_; }
        bool IsVideoPlayerRunning() const;
        bool IsAudioPlayerRunning() const;

        // Recovery and error handling
        void ResetStreams();
        void RecoverFromDiscontinuity();
        bool HasStreamErrors() const;
        std::vector<std::wstring> GetLastErrors() const;

    private:
        // Configuration
        DemuxConfig config_;
        std::function<void(const std::wstring&)> log_callback_;

        // Demuxing state
        std::atomic<bool> demuxing_active_{false};
        std::unique_ptr<TSDemux::AVContext> av_context_;
        
        // Stream management
        mutable std::mutex streams_mutex_;
        std::map<uint16_t, StreamInfo> available_streams_;
        std::map<uint16_t, bool> enabled_streams_;
        
        // Input buffering (for HLS segments)
        std::mutex input_buffer_mutex_;
        std::vector<uint8_t> input_buffer_;
        uint64_t buffer_position_ = 0;
        uint64_t buffer_read_position_ = 0;
        
        // Output packet queues
        std::mutex video_queue_mutex_;
        std::mutex audio_queue_mutex_;
        std::queue<DemuxedPacket> video_packet_queue_;
        std::queue<DemuxedPacket> audio_packet_queue_;
        
        // Worker threads
        std::thread hls_downloader_thread_;
        std::thread demux_processor_thread_;
        std::thread video_output_thread_;
        std::thread audio_output_thread_;
        
        // Player processes
        HANDLE video_player_process_ = nullptr;
        HANDLE audio_player_process_ = nullptr;
        HANDLE video_player_stdin_ = nullptr;
        HANDLE audio_player_stdin_ = nullptr;
        
        // Statistics and monitoring
        mutable std::mutex stats_mutex_;
        DemuxStats stats_;
        std::chrono::steady_clock::time_point demux_start_time_;
        
        // Error handling
        mutable std::mutex errors_mutex_;
        std::vector<std::wstring> recent_errors_;
        std::chrono::steady_clock::time_point last_error_time_;
        
        // Thread functions
        void HLSDownloaderThread(const std::wstring& playlist_url, std::atomic<bool>& cancel_token);
        void DemuxProcessorThread(std::atomic<bool>& cancel_token);
        void VideoOutputThread(std::atomic<bool>& cancel_token);
        void AudioOutputThread(std::atomic<bool>& cancel_token);
        
        // Stream processing
        bool ProcessDemuxedPacket(const TSDemux::STREAM_PKT& pkt);
        void UpdateStreamInfo(uint16_t pid, TSDemux::ElementaryStream* es);
        StreamType DetermineStreamType(const TSDemux::ElementaryStream* es) const;
        
        // Player management
        bool LaunchVideoPlayer();
        bool LaunchAudioPlayer();
        void TerminateVideoPlayer();
        void TerminateAudioPlayer();
        bool SendVideoPacket(const DemuxedPacket& packet);
        bool SendAudioPacket(const DemuxedPacket& packet);
        
        // Utility functions
        void LogMessage(const std::wstring& message);
        void LogError(const std::wstring& error);
        void UpdateStats();
        bool FetchHLSSegment(const std::wstring& segment_url, std::vector<uint8_t>& data, std::atomic<bool>* cancel_token);
        std::vector<std::wstring> ParseHLSPlaylist(const std::string& playlist_content, const std::wstring& base_url);
        
        // Buffer management
        void AppendToInputBuffer(const std::vector<uint8_t>& data);
        bool IsInputBufferReady(uint64_t pos, size_t len) const;
        void ClearOldBufferData();
        
        // Stream health monitoring
        void CheckStreamHealth();
        void HandleStreamTimeout(uint16_t pid);
        void RestartFailedStreams();
    };

    // Factory function for creating demux wrapper with appropriate configuration
    std::unique_ptr<DemuxMpegtsWrapper> CreateDemuxWrapper(
        const std::wstring& player_path = L"mpv.exe",
        bool enable_separate_streams = true,
        bool enable_debug_logging = false
    );

} // namespace tardsplaya