#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

// Forward declarations for GPAC types to avoid including full headers
struct GF_Terminal;
struct GF_User;
struct GF_Config;

// GPAC-based video player for built-in streaming
class GpacPlayer {
public:
    GpacPlayer();
    ~GpacPlayer();
    
    // Initialize GPAC player with optional parent window for embedding
    // If parentWindow is nullptr, creates a separate window
    bool Initialize(HWND parentWindow, const std::wstring& channelName);
    
    // Start playing a stream URL
    bool Play(const std::wstring& streamUrl);
    
    // Stop playback
    void Stop();
    
    // Pause/Resume playback
    void Pause();
    void Resume();
    
    // Check if currently playing
    bool IsPlaying() const;
    
    // Get the video window handle for embedding
    HWND GetVideoWindow() const;
    
    // Set callback for ad detection events
    void SetAdDetectionCallback(std::function<void(bool)> callback);
    
    // Show/hide "Skipping ads" overlay
    void ShowAdSkippingMessage(bool show);
    
    // Resize the video window
    void Resize(int width, int height);
    
    // Handle discontinuities (GPAC handles this better than external players)
    void HandleDiscontinuity();
    
    // Process MPEG-TS data packets
    bool ProcessMpegTsData(const uint8_t* data, size_t dataSize);
    
    // Get GPAC terminal for direct data feeding
    void* GetTerminal() const { return m_terminal; }
    
    // Set log callback for debugging
    void SetLogCallback(std::function<void(const std::wstring&)> callback);

private:
    // GPAC components
    GF_Terminal* m_terminal;
    GF_User* m_user;
    GF_Config* m_config;
    
    // Windows components
    HWND m_parentWindow;
    HWND m_videoWindow;
    HWND m_overlayWindow;
    
    // State
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_playing;
    std::atomic<bool> m_paused;
    bool m_useSeparateWindow;
    std::wstring m_channelName;
    std::wstring m_currentUrl;
    
    // Callbacks
    std::function<void(bool)> m_adDetectionCallback;
    std::function<void(const std::wstring&)> m_logCallback;
    
    // Internal methods
    bool InitializeGpac();
    void CleanupGpac();
    bool CreateVideoWindow();
    bool CreateOverlayWindow();
    void UpdateOverlay();
    void LogMessage(const std::wstring& message);
    
    // GPAC callbacks
    static void OnGpacLog(void* cbck, int level, int tool, const char* message);
    static void OnGpacEvent(void* user_data, int event_type, int error, const char* message);
};

// Factory function to create GPAC player instances
std::unique_ptr<GpacPlayer> CreateGpacPlayer();