#pragma once
// DirectShow Events Support for Enhanced Discontinuity Handling
// Provides media player buffer control during ad breaks and discontinuities

#include <windows.h>
#include <dshow.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "quartz.lib")

namespace directshow_events {

    // DirectShow Event Types for discontinuity handling
    enum class MediaEvent {
        SEGMENT_STARTED,        // New segment/discontinuity detected
        BUFFER_CLEAR_REQUEST,   // Request to clear video buffers
        PLAYBACK_RESUMED,       // Normal playback resumed after discontinuity
        GRAPH_READY,           // DirectShow graph is ready for events
        ERROR_OCCURRED         // Error in DirectShow processing
    };

    // Callback function type for media events
    using MediaEventCallback = std::function<void(MediaEvent event, const std::wstring& description)>;

    // DirectShow Filter Graph Manager for enhanced buffer control
    class DirectShowController {
    public:
        DirectShowController();
        ~DirectShowController();

        // Initialize DirectShow graph for media player integration
        bool Initialize(const std::wstring& player_name, MediaEventCallback callback = nullptr);
        
        // Check if a media player supports DirectShow integration
        static bool IsDirectShowCompatible(const std::wstring& player_path);
        
        // Send buffer clear event to media player during discontinuities
        bool ClearVideoBuffers();
        
        // Send segment transition event to prepare for ad break recovery
        bool NotifySegmentTransition();
        
        // Reset video rendering pipeline for clean restart
        bool ResetVideoRenderer();
        
        // Check if DirectShow graph is ready for operations
        bool IsGraphReady() const { return graph_ready_; }
        
        // Get last error message
        std::wstring GetLastError() const { return last_error_; }
        
        // Event processing thread management
        void StartEventProcessing();
        void StopEventProcessing();
        
    private:
        // DirectShow COM interfaces
        IGraphBuilder* graph_builder_;
        IMediaControl* media_control_;
        IMediaEventEx* media_event_;
        IBaseFilter* video_renderer_;
        
        // Event processing
        std::atomic<bool> event_processing_active_;
        std::thread event_thread_;
        MediaEventCallback event_callback_;
        
        // State tracking
        std::atomic<bool> graph_ready_;
        std::wstring last_error_;
        std::wstring player_name_;
        
        // Internal methods
        bool CreateFilterGraph();
        bool FindVideoRenderer();
        void ProcessMediaEvents();
        void LogEvent(MediaEvent event, const std::wstring& description);
        
        // Buffer control methods
        bool FlushVideoRenderer();
        bool ResetRendererState();
    };

    // Utility functions for DirectShow integration
    namespace utils {
        
        // Get recommended DirectShow-compatible media player
        std::wstring GetPreferredDirectShowPlayer();
        
        // Check if current system supports DirectShow events
        bool IsDirectShowAvailable();
        
        // Create DirectShow-compatible command line for media player
        std::wstring CreateDirectShowCommandLine(const std::wstring& player_path, 
                                                const std::wstring& input_source);
        
        // Register custom DirectShow event for buffer clearing
        bool RegisterCustomBufferClearEvent();
        
        // Send Windows message to DirectShow-compatible player for buffer control
        bool SendBufferClearMessage(HWND player_window);
    }

    // Enhanced Media Player Launcher with DirectShow support
    class DirectShowMediaPlayer {
    public:
        DirectShowMediaPlayer();
        ~DirectShowMediaPlayer();
        
        // Launch media player with DirectShow integration
        bool Launch(const std::wstring& player_path, 
                   const std::wstring& input_source,
                   MediaEventCallback event_callback = nullptr);
        
        // Send discontinuity event to clear buffers
        bool HandleDiscontinuity();
        
        // Check if player is running and responsive
        bool IsPlayerHealthy() const;
        
        // Get player process handle
        HANDLE GetPlayerProcess() const { return player_process_; }
        
        // Stop player and cleanup DirectShow resources
        void Stop();
        
    private:
        std::unique_ptr<DirectShowController> ds_controller_;
        HANDLE player_process_;
        HWND player_window_;
        std::wstring player_path_;
        std::atomic<bool> directshow_enabled_;
        
        // Player window detection
        bool FindPlayerWindow();
        static BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam);
    };

} // namespace directshow_events