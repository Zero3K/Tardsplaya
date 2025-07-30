#pragma once

// TS Demuxer Stream Implementation for Tardsplaya
// Integrates ts_demuxer to separate video and audio streams for better discontinuity recovery

#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Include ts_demuxer
extern "C" {
#include "ts_demuxer.h"
#include "es_output.h"
}

// Forward declarations
void AddDebugLog(const std::wstring& msg);

namespace tardsplaya {

// TS Demuxer streaming manager
class TSDemuxerStreamManager {
public:
    TSDemuxerStreamManager(const std::wstring& player_path, const std::wstring& channel_name);
    ~TSDemuxerStreamManager();
    
    // Initialize the streaming system
    bool Initialize();
    
    // Start streaming with demuxed video/audio
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
    
    // Get streaming statistics
    struct DemuxerStats {
        uint64_t segments_processed;
        uint64_t video_packets;
        uint64_t audio_packets;
        uint64_t bytes_transferred;
        bool player_running;
        bool demuxer_active;
    };
    DemuxerStats GetStats() const;
    
private:
    std::wstring player_path_;
    std::wstring channel_name_;
    
    std::atomic<bool> streaming_active_{false};
    std::atomic<bool> should_stop_{false};
    std::atomic<uint64_t> segments_processed_{0};
    std::atomic<uint64_t> video_packets_{0};
    std::atomic<uint64_t> audio_packets_{0};
    std::atomic<uint64_t> bytes_transferred_{0};
    
    HANDLE player_process_;
    HANDLE video_pipe_;
    HANDLE audio_pipe_;
    PROCESS_INFORMATION process_info_;
    
    // File paths for separate video/audio output
    std::wstring video_file_path_;
    std::wstring audio_file_path_;
    HANDLE video_file_handle_;
    HANDLE audio_file_handle_;
    
    std::thread streaming_thread_;
    
    // Callbacks and state
    std::function<void(const std::wstring&)> log_callback_;
    std::atomic<int>* chunk_count_ptr_;
    std::atomic<bool>* cancel_token_ptr_;
    
    // Thread functions
    void StreamingThreadFunction(const std::wstring& playlist_url);
    
    // Helper functions
    bool StartPlayerWithFiles();
    bool CreateTempFiles();
    void CleanupTempFiles();
    bool DownloadPlaylistSegments(const std::wstring& playlist_url, std::vector<std::wstring>& segment_urls);
    bool DownloadSegment(const std::wstring& segment_url, std::vector<char>& segment_data);
    bool ProcessSegmentWithDemuxer(const std::vector<char>& segment_data);
    void LogMessage(const std::wstring& message);
    void UpdateChunkCount(int count);
    void Cleanup();
    
    // Memory-based ES output implementation
    class MemoryESOutput {
    public:
        MemoryESOutput(ES_OUTPUT_TYPE type, HANDLE pipe_handle);
        ~MemoryESOutput();
        
        bool WriteData(const unsigned char* data, unsigned int length);
        ES_OUTPUT_TYPE GetType() const { return type_; }
        
    private:
        ES_OUTPUT_TYPE type_;
        HANDLE pipe_handle_;
        std::atomic<uint64_t> bytes_written_{0};
    };
    
    std::unique_ptr<MemoryESOutput> video_output_;
    std::unique_ptr<MemoryESOutput> audio_output_;
};

// Memory-based TS demuxer implementation
class MemoryTSDemuxer {
public:
    MemoryTSDemuxer();
    ~MemoryTSDemuxer();
    
    // Initialize with memory buffer instead of file
    bool Initialize(const char* buffer, size_t buffer_size);
    
    // Set output handlers for video and audio streams
    bool SetVideoOutput(std::function<bool(const unsigned char*, unsigned int)> video_handler);
    bool SetAudioOutput(std::function<bool(const unsigned char*, unsigned int)> audio_handler);
    
    // Process the TS data
    bool Process();
    
    // Get processing statistics
    struct ProcessStats {
        unsigned int packets_processed;
        unsigned int video_pid;
        unsigned int audio_pid;
        unsigned int pcr_pid;
        bool has_video;
        bool has_audio;
    };
    ProcessStats GetStats() const;
    
private:
    const char* buffer_;
    size_t buffer_size_;
    size_t current_offset_;
    unsigned int packet_size_;
    
    // Stream information
    unsigned int pmt_pid_;
    unsigned int pcr_pid_;
    unsigned int video_pid_;
    unsigned int audio_pid_;
    
    // Output handlers
    std::function<bool(const unsigned char*, unsigned int)> video_handler_;
    std::function<bool(const unsigned char*, unsigned int)> audio_handler_;
    
    // Statistics
    ProcessStats stats_;
    
    // Helper functions
    bool DetectPacketSize();
    bool ParseTSPacket(const unsigned char* packet);
    bool ParsePAT(const unsigned char* payload, unsigned int length);
    bool ParsePMT(const unsigned char* payload, unsigned int length);
    bool ParsePES(const unsigned char* payload, unsigned int length, unsigned int pid, bool unit_start);
};

} // namespace tardsplaya

// C++ wrapper functions for ts_demuxer integration
std::thread StartTSDemuxerThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback = nullptr,
    const std::wstring& channel_name = L"",
    std::atomic<int>* chunk_count = nullptr,
    HWND main_window = nullptr,
    size_t tab_index = 0,
    HANDLE* player_process_handle = nullptr
);