#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>

// Simplified built-in player that processes streams internally without external players
class SimpleBuiltinPlayer {
public:
    SimpleBuiltinPlayer();
    ~SimpleBuiltinPlayer();

    // Initialize the player with a target window for status display
    bool Initialize(HWND hwndStatus);
    
    // Start processing a stream internally
    bool StartStream(const std::wstring& streamName);
    
    // Stop the stream
    void StopStream();
    
    // Feed raw stream data (TS segments) to the player for processing
    bool FeedData(const char* data, size_t size);
    
    // Check if player is currently processing
    bool IsPlaying() const;
    
    // Get statistics
    size_t GetTotalBytesProcessed() const;
    size_t GetSegmentsProcessed() const;
    
    // Update display window with current status
    void UpdateStatus();
    
    // Cleanup resources
    void Cleanup();

private:
    // Window handle for status display
    HWND m_hwndStatus;
    
    // Processing state
    std::atomic<bool> m_isPlaying;
    std::atomic<bool> m_isInitialized;
    
    // Stream data processing
    std::queue<std::vector<char>> m_dataBuffer;
    std::mutex m_bufferMutex;
    std::thread m_processThread;
    std::atomic<bool> m_processRunning;
    
    // Statistics
    std::atomic<size_t> m_totalBytesProcessed;
    std::atomic<size_t> m_segmentsProcessed;
    std::wstring m_currentStreamName;
    
    // Internal methods
    void ProcessThreadProc();
    void ProcessSegment(const std::vector<char>& data);
    std::wstring FormatStatus() const;
};