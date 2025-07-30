#pragma once

// MPEG-TS Demux Integration for Tardsplaya
// Provides demuxing of video and audio streams for better discontinuity recovery
// Integrates janbar/demux-mpegts library

#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <memory>
#include <map>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Include demux-mpegts headers
#include "demux-mpegts/src/tsDemuxer.h"
#include "demux-mpegts/src/elementaryStream.h"
#include "demux-mpegts/src/debug.h"

// Forward declarations
void AddDebugLog(const std::wstring& msg);

namespace tardsplaya {

// Elementary stream data structure
struct ElementaryStreamData {
    uint16_t pid;
    TSDemux::STREAM_TYPE stream_type;
    std::wstring codec_name;
    std::wstring file_extension;
    std::wstring output_filename;
    std::ofstream output_file;
    bool is_audio;
    bool is_video;
    uint64_t bytes_written;
    
    ElementaryStreamData() : pid(0), stream_type(TSDemux::STREAM_TYPE_UNKNOWN), 
                           is_audio(false), is_video(false), bytes_written(0) {}
};

// Custom demuxer implementation that extends TSDemux::TSDemuxer
class MpegTSDemuxer : public TSDemux::TSDemuxer {
public:
    MpegTSDemuxer(const std::wstring& channel_name, const std::wstring& output_dir);
    virtual ~MpegTSDemuxer();
    
    // TSDemuxer interface implementation
    virtual const unsigned char* ReadAV(uint64_t pos, size_t len) override;
    
    // Initialize demuxer with buffer
    bool Initialize(size_t buffer_size = 1024 * 1024); // 1MB default
    
    // Process MPEG-TS data
    bool ProcessTSData(const char* data, size_t size);
    
    // Get detected streams
    const std::map<uint16_t, ElementaryStreamData>& GetStreams() const { return streams_; }
    
    // Get video and audio output filenames for media player
    std::wstring GetVideoOutputFile() const { return video_output_file_; }
    std::wstring GetAudioOutputFile() const { return audio_output_file_; }
    
    // Check if demuxing is active
    bool IsActive() const { return is_active_; }
    
    // Stop demuxing and close files
    void Stop();
    
    // Get statistics
    struct DemuxStats {
        uint64_t total_bytes_processed;
        uint64_t video_bytes_written;
        uint64_t audio_bytes_written;
        size_t video_streams_count;
        size_t audio_streams_count;
        uint16_t primary_video_pid;
        uint16_t primary_audio_pid;
    };
    DemuxStats GetStats() const;

private:
    std::wstring channel_name_;
    std::wstring output_dir_;
    std::unique_ptr<TSDemux::AVContext> av_context_;
    
    // Buffer management
    std::unique_ptr<unsigned char[]> buffer_;
    size_t buffer_size_;
    size_t buffer_pos_;
    uint64_t stream_pos_;
    
    // Stream management
    std::map<uint16_t, ElementaryStreamData> streams_;
    std::wstring video_output_file_;
    std::wstring audio_output_file_;
    uint16_t primary_video_pid_;
    uint16_t primary_audio_pid_;
    
    // State
    bool is_active_;
    uint64_t total_bytes_processed_;
    
    // Helper functions
    void OnStreamData(uint16_t pid, TSDemux::STREAM_TYPE stream_type, 
                      const unsigned char* data, size_t size);
    void CreateOutputStream(uint16_t pid, TSDemux::STREAM_TYPE stream_type);
    std::wstring GetCodecName(TSDemux::STREAM_TYPE stream_type);
    std::wstring GetFileExtension(TSDemux::STREAM_TYPE stream_type);
    std::wstring GenerateOutputFilename(uint16_t pid, const std::wstring& extension);
    void SelectPrimaryStreams();
};

// High-level demux stream manager
class DemuxStreamManager {
public:
    DemuxStreamManager(const std::wstring& player_path, const std::wstring& channel_name);
    ~DemuxStreamManager();
    
    // Initialize the demux system
    bool Initialize();
    
    // Start demux streaming
    bool StartStreaming(
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        std::function<void(const std::wstring&)> log_callback = nullptr,
        std::atomic<int>* chunk_count = nullptr
    );
    
    // Stop streaming
    void StopStreaming();
    
    // Check if streaming is active
    bool IsStreaming() const { return streaming_active_.load(); }
    
    // Get player process handle
    HANDLE GetPlayerProcess() const { return player_process_; }
    
    // Get demux statistics
    MpegTSDemuxer::DemuxStats GetDemuxStats() const;

private:
    std::wstring player_path_;
    std::wstring channel_name_;
    std::wstring output_dir_;
    std::unique_ptr<MpegTSDemuxer> demuxer_;
    
    std::atomic<bool> streaming_active_{false};
    std::atomic<bool> should_stop_{false};
    
    std::thread downloader_thread_;
    std::thread demux_thread_;
    std::thread player_thread_;
    
    // Player management
    HANDLE player_process_;
    PROCESS_INFORMATION process_info_;
    
    // Callbacks and state
    std::function<void(const std::wstring&)> log_callback_;
    std::atomic<int>* chunk_count_ptr_;
    std::atomic<bool>* cancel_token_ptr_;
    
    // Thread functions
    void DownloadThreadFunction(const std::wstring& playlist_url);
    void DemuxThreadFunction();
    void PlayerThreadFunction();
    
    // Helper functions
    bool CreateOutputDirectory();
    bool LaunchMediaPlayer();
    std::wstring GetPlayerCommandLine();
    bool DownloadPlaylistSegments(const std::wstring& playlist_url, std::vector<std::wstring>& segment_urls);
    bool DownloadSegment(const std::wstring& segment_url, std::vector<char>& segment_data);
    void LogMessage(const std::wstring& message);
    void UpdateChunkCount(int count);
    std::wstring GenerateUniqueOutputDirectory();
};

// Utility functions for media player command line generation
class MediaPlayerCommandBuilder {
public:
    static std::wstring BuildMPCCommand(const std::wstring& player_path, 
                                       const std::wstring& video_file, 
                                       const std::wstring& audio_file);
    
    static std::wstring BuildVLCCommand(const std::wstring& player_path, 
                                       const std::wstring& video_file, 
                                       const std::wstring& audio_file);
    
    static std::wstring BuildMPVCommand(const std::wstring& player_path, 
                                       const std::wstring& video_file, 
                                       const std::wstring& audio_file);
    
    static std::wstring DetectPlayerType(const std::wstring& player_path);
};

} // namespace tardsplaya