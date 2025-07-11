#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX      // Prevent Windows min/max macros
#endif
#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>

// Media Foundation headers for video playback
#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <evr.h>
#include <d3d9.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "evr.lib")
#pragma comment(lib, "d3d9.lib")

// Simplified built-in player that processes streams internally without external players
class SimpleBuiltinPlayer {
public:
    SimpleBuiltinPlayer();
    ~SimpleBuiltinPlayer();

    // Initialize the player with a target window for status display
    bool Initialize(HWND hwndStatus);
    
    // Start processing a stream internally with separate video window
    bool StartStream(const std::wstring& streamName, const std::wstring& quality = L"");
    
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
    
    // Get the video window handle (for separate window mode)
    HWND GetVideoWindow() const;
    
    // Cleanup resources
    void Cleanup();

private:
    // Window handle for status display
    HWND m_hwndStatus;
    
    // Separate video window for stream display
    HWND m_hwndVideo;
    
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
    std::wstring m_currentQuality;
    
    // Media Foundation components for video rendering
    IMFMediaSession* m_pSession;
    IMFVideoDisplayControl* m_pVideoDisplay;
    IMFByteStream* m_pByteStream;
    IMFSourceResolver* m_pSourceResolver;
    IMFMediaSource* m_pMediaSource;
    IMFTopology* m_pTopology;
    std::atomic<bool> m_mfInitialized;
    
    // Internal methods
    void ProcessThreadProc();
    void ProcessSegment(const std::vector<char>& data);
    std::wstring FormatStatus() const;
    
    // Video window management
    bool CreateVideoWindow(const std::wstring& quality);
    void DestroyVideoWindow();
    std::pair<int, int> ParseQualitySize(const std::wstring& quality);
    static LRESULT CALLBACK VideoWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    
    // Media Foundation methods
    bool InitializeMediaFoundation();
    void CleanupMediaFoundation();
    bool CreateVideoSession();
    bool SetVideoWindow(HWND hwnd);
    bool StartVideoPlayback();
};