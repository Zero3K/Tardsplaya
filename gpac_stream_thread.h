#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <thread>
#include <memory>

// Forward declaration
class GpacPlayer;

// GPAC-based streaming thread that feeds data directly to GPAC player
// instead of external media player processes
class GpacStreamThread {
public:
    GpacStreamThread(
        std::shared_ptr<GpacPlayer> gpacPlayer,
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        std::function<void(const std::wstring&)> log_callback,
        const std::wstring& channel_name,
        std::atomic<int>* chunk_count = nullptr
    );
    
    ~GpacStreamThread();
    
    // Start the streaming thread
    bool Start();
    
    // Stop the streaming thread
    void Stop();
    
    // Check if thread is running
    bool IsRunning() const;

private:
    std::shared_ptr<GpacPlayer> m_gpacPlayer;
    std::wstring m_playlistUrl;
    std::atomic<bool>& m_cancelToken;
    std::function<void(const std::wstring&)> m_logCallback;
    std::wstring m_channelName;
    std::atomic<int>* m_chunkCount;
    
    std::thread m_thread;
    std::atomic<bool> m_running;
    
    // Main streaming loop
    void StreamingLoop();
    
    // Download and parse HLS playlist
    bool UpdatePlaylist(std::vector<std::wstring>& segments);
    
    // Download a media segment
    bool DownloadSegment(const std::wstring& segmentUrl, std::vector<uint8_t>& data);
    
    // Feed data to GPAC player
    bool FeedDataToGpac(const std::vector<uint8_t>& data);
    
    // Detect ad segments (for ad-skipping functionality)
    bool IsAdSegment(const std::wstring& segmentUrl, const std::vector<uint8_t>& data);
    
    // Handle discontinuities in the stream
    void HandleDiscontinuity();
    
    // Log a message
    void LogMessage(const std::wstring& message);
};

// Factory function to create GPAC stream thread
std::unique_ptr<GpacStreamThread> CreateGpacStreamThread(
    std::shared_ptr<GpacPlayer> gpacPlayer,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count = nullptr
);