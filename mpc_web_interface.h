#pragma once
// MPC-HC Web Interface Controller for Tardsplaya
// Implements HTTP-based control to prevent freezing during discontinuities

#include <string>
#include <chrono>
#include <atomic>
#include <memory>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

class MPCWebInterface {
public:
    MPCWebInterface();
    ~MPCWebInterface();
    
    // Configuration for MPC-HC web interface
    struct WebConfig {
        int port = 13579;                    // Default MPC-HC web interface port
        std::wstring host = L"127.0.0.1";   // Local interface only
        std::chrono::milliseconds timeout{3000}; // HTTP request timeout
        int max_retries = 3;                 // Max retry attempts for commands
        std::chrono::milliseconds retry_delay{500}; // Delay between retries
    };
    
    // Initialize connection to MPC-HC web interface
    bool Initialize(const WebConfig& config = WebConfig{});
    
    // Check if web interface is available and responding
    bool IsAvailable() const;
    
    // Get current player state
    enum class PlayerState {
        Unknown,
        Stopped,
        Playing,
        Paused,
        Loading
    };
    PlayerState GetPlayerState() const;
    
    // Discontinuity recovery commands
    bool HandleDiscontinuity();             // Main discontinuity recovery function
    bool PausePlayback();                   // Pause before discontinuity
    bool ResumePlayback();                  // Resume after discontinuity
    bool SeekToBeginning();                 // Seek to start if frozen
    bool RefreshStream();                   // Force stream refresh
    
    // Player control commands
    bool SendPlayCommand();
    bool SendPauseCommand();
    bool SendStopCommand();
    bool SendCloseCommand();
    
    // Stream management
    bool OpenURL(const std::wstring& url);  // Open new stream URL
    bool GetPosition(double& position) const; // Get current position in seconds
    bool GetDuration(double& duration) const; // Get total duration in seconds
    
    // Configuration
    void SetConfig(const WebConfig& config) { config_ = config; }
    const WebConfig& GetConfig() const { return config_; }
    
private:
    WebConfig config_;
    std::atomic<bool> initialized_{false};
    mutable std::atomic<bool> available_{false};
    mutable std::chrono::steady_clock::time_point last_check_time_;
    mutable std::chrono::milliseconds check_interval_{5000}; // Check availability every 5 seconds
    
    // HTTP communication
    bool SendHTTPCommand(const std::wstring& command, std::wstring& response) const;
    bool SendHTTPRequest(const std::wstring& path, const std::wstring& method, 
                        const std::wstring& data, std::wstring& response) const;
    
    // Response parsing
    PlayerState ParsePlayerState(const std::wstring& response) const;
    bool ParsePosition(const std::wstring& response, double& position) const;
    bool ParseDuration(const std::wstring& response, double& duration) const;
    
    // Utility functions
    std::wstring UrlEncode(const std::wstring& text) const;
    bool IsValidResponse(const std::wstring& response) const;
    
    // Availability checking
    void CheckAvailability() const;
    bool TestConnection() const;
};

// Factory function to create MPC-HC web interface if MPC-HC is detected
std::unique_ptr<MPCWebInterface> CreateMPCWebInterface(const std::wstring& player_path);

// Utility function to check if player path is MPC-HC
bool IsMPCHC(const std::wstring& player_path);

// Forward declaration for debug logging
void AddDebugLog(const std::wstring& msg);