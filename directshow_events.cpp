#include "directshow_events.h"
#include <comdef.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>

// External debug logging function
extern void AddDebugLog(const std::wstring& msg);

namespace directshow_events {

// DirectShowController Implementation
DirectShowController::DirectShowController() 
    : event_callback_(nullptr)
    , graph_ready_(false) {
    
    // Initialize COM for DirectShow
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}

DirectShowController::~DirectShowController() {
    // For external player communication, we just need to cleanup COM
    CoUninitialize();
}

bool DirectShowController::Initialize(const std::wstring& player_name, MediaEventCallback callback) {
    player_name_ = player_name;
    event_callback_ = callback;
    
    LogEvent(MediaEvent::GRAPH_READY, L"Initializing DirectShow event sender for " + player_name);
    
    // For external media players, we don't create our own DirectShow graph
    // Instead, we prepare to send events to the external player
    if (!utils::IsDirectShowAvailable()) {
        last_error_ = L"DirectShow components not available on system";
        LogEvent(MediaEvent::ERROR_OCCURRED, last_error_);
        return false;
    }
    
    // Register custom buffer clear event for inter-process communication
    if (!utils::RegisterCustomBufferClearEvent()) {
        last_error_ = L"Failed to register custom buffer clear event";
        LogEvent(MediaEvent::ERROR_OCCURRED, last_error_);
        return false;
    }
    
    graph_ready_ = true;
    LogEvent(MediaEvent::GRAPH_READY, L"DirectShow event sender initialized successfully");
    
    return true;
}

bool DirectShowController::IsDirectShowCompatible(const std::wstring& player_path) {
    // Check for known DirectShow-compatible media players
    std::wstring lower_path = player_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::towlower);
    
    // MPC-HC is the most DirectShow-compatible
    if (lower_path.find(L"mpc-hc") != std::wstring::npos ||
        lower_path.find(L"mpc-be") != std::wstring::npos ||
        lower_path.find(L"mpc64") != std::wstring::npos) {
        return true;
    }
    
    // Windows Media Player (legacy but supports DirectShow)
    if (lower_path.find(L"wmplayer") != std::wstring::npos) {
        return true;
    }
    
    // VLC has some DirectShow integration
    if (lower_path.find(L"vlc") != std::wstring::npos) {
        return true;
    }
    
    // mpv, ffplay don't use DirectShow
    if (lower_path.find(L"mpv") != std::wstring::npos ||
        lower_path.find(L"ffplay") != std::wstring::npos) {
        return false;
    }
    
    return false; // Default to false for unknown players
}

bool DirectShowController::ClearVideoBuffers() {
    if (!graph_ready_) {
        last_error_ = L"DirectShow event sender not ready for buffer clearing";
        return false;
    }
    
    LogEvent(MediaEvent::BUFFER_CLEAR_REQUEST, L"Sending buffer clear request to external media player");
    
    // For external media players, we send buffer clear events rather than controlling our own graph
    bool success = false;
    
    // Method 1: Try to find the media player window and send custom message
    HWND player_window = FindPlayerWindow();
    if (player_window) {
        success = utils::SendBufferClearMessage(player_window);
        if (success) {
            LogEvent(MediaEvent::BUFFER_CLEAR_REQUEST, L"Buffer clear message sent via Windows message");
        }
    }
    
    // Method 2: For MPC-HC, we can also try sending keyboard shortcuts
    if (!success && player_name_.find(L"mpc-hc") != std::wstring::npos) {
        // Send Ctrl+R (refresh) to MPC-HC to clear buffers
        if (player_window) {
            PostMessage(player_window, WM_KEYDOWN, VK_CONTROL, 0);
            PostMessage(player_window, WM_KEYDOWN, 'R', 0);
            PostMessage(player_window, WM_KEYUP, 'R', 0);
            PostMessage(player_window, WM_KEYUP, VK_CONTROL, 0);
            success = true;
            LogEvent(MediaEvent::BUFFER_CLEAR_REQUEST, L"Buffer clear sent via MPC-HC refresh shortcut");
        }
    }
    
    if (success) {
        LogEvent(MediaEvent::BUFFER_CLEAR_REQUEST, L"Video buffer clear operation completed");
    } else {
        LogEvent(MediaEvent::ERROR_OCCURRED, L"Failed to send buffer clear request to media player");
    }
    
    return success;
}

bool DirectShowController::NotifySegmentTransition() {
    if (!graph_ready_) {
        return false;
    }
    
    LogEvent(MediaEvent::SEGMENT_STARTED, L"Notifying DirectShow of segment transition");
    
    // For external players, we send window messages instead of DirectShow events
    // This is handled through the SendPlayerCommand method
    
    return true;
}

bool DirectShowController::ResetVideoRenderer() {
    LogEvent(MediaEvent::PLAYBACK_RESUMED, L"Requesting video renderer reset for external player");
    
    // For external players, we can't directly reset their renderers
    // But we can request a refresh or restart
    bool success = false;
    
    // Try to send refresh command to player
    HWND player_window = FindPlayerWindow();
    if (player_window) {
        if (player_name_.find(L"mpc-hc") != std::wstring::npos) {
            // MPC-HC: Send F5 (refresh)
            PostMessage(player_window, WM_KEYDOWN, VK_F5, 0);
            PostMessage(player_window, WM_KEYUP, VK_F5, 0);
            success = true;
            LogEvent(MediaEvent::PLAYBACK_RESUMED, L"MPC-HC refresh command sent");
        } else if (player_name_.find(L"vlc") != std::wstring::npos) {
            // VLC: Send Ctrl+R (refresh)
            PostMessage(player_window, WM_KEYDOWN, VK_CONTROL, 0);
            PostMessage(player_window, WM_KEYDOWN, 'R', 0);
            PostMessage(player_window, WM_KEYUP, 'R', 0);
            PostMessage(player_window, WM_KEYUP, VK_CONTROL, 0);
            success = true;
            LogEvent(MediaEvent::PLAYBACK_RESUMED, L"VLC refresh command sent");
        }
    }
    
    return success;
}

void DirectShowController::LogEvent(MediaEvent event, const std::wstring& description) {
    if (event_callback_) {
        event_callback_(event, description);
    }
    
    // Also log to debug system
    AddDebugLog(L"[DIRECTSHOW_EVENT] " + description);
}

HWND DirectShowController::FindPlayerWindow() {
    // Find the media player window by process name
    HWND player_window = nullptr;
    
    if (player_name_.find(L"mpc-hc") != std::wstring::npos) {
        player_window = FindWindow(L"MediaPlayerClassicW", nullptr);
        if (!player_window) {
            player_window = FindWindow(L"MPC-HC", nullptr);
        }
    } else if (player_name_.find(L"vlc") != std::wstring::npos) {
        player_window = FindWindow(L"Qt5QWindowIcon", nullptr);
        if (!player_window) {
            player_window = FindWindow(L"VLC media player", nullptr);
        }
    }
    
    return player_window;
}

bool DirectShowController::SendPlayerCommand(const std::wstring& command) {
    HWND player_window = FindPlayerWindow();
    if (!player_window) {
        return false;
    }
    
    // Send command based on player type
    if (command == L"refresh") {
        if (player_name_.find(L"mpc-hc") != std::wstring::npos) {
            PostMessage(player_window, WM_KEYDOWN, VK_F5, 0);
            PostMessage(player_window, WM_KEYUP, VK_F5, 0);
            return true;
        } else if (player_name_.find(L"vlc") != std::wstring::npos) {
            PostMessage(player_window, WM_KEYDOWN, VK_CONTROL, 0);
            PostMessage(player_window, WM_KEYDOWN, 'R', 0);
            PostMessage(player_window, WM_KEYUP, 'R', 0);
            PostMessage(player_window, WM_KEYUP, VK_CONTROL, 0);
            return true;
        }
    }
    
    return false;
}

// Utility functions implementation
namespace utils {

std::wstring GetPreferredDirectShowPlayer() {
    // Check for MPC-HC first (best DirectShow support)
    std::vector<std::wstring> mpc_paths = {
        L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe",
        L"C:\\Program Files (x86)\\MPC-HC\\mpc-hc.exe",
        L"mpc-hc64.exe",
        L"mpc-hc.exe"
    };
    
    for (const auto& path : mpc_paths) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    
    // Check for VLC (limited DirectShow support)
    std::vector<std::wstring> vlc_paths = {
        L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe",
        L"C:\\Program Files (x86)\\VideoLAN\\VLC\\vlc.exe",
        L"vlc.exe"
    };
    
    for (const auto& path : vlc_paths) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    
    return L""; // No DirectShow-compatible player found
}

bool IsDirectShowAvailable() {
    // For external player communication, we don't need to create DirectShow components
    // We just need to verify that Windows messaging system is available for IPC
    // This is always available on Windows systems
    return true;
}

std::wstring CreateDirectShowCommandLine(const std::wstring& player_path, const std::wstring& input_source) {
    std::wstring lower_path = player_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::towlower);
    
    if (lower_path.find(L"mpc-hc") != std::wstring::npos) {
        // MPC-HC with enhanced DirectShow options
        return L"\"" + player_path + L"\" \"" + input_source + L"\" /new /nofocus /minimized";
    } else if (lower_path.find(L"vlc") != std::wstring::npos) {
        // VLC with DirectShow input module
        return L"\"" + player_path + L"\" \"" + input_source + L"\" --intf dummy --no-one-instance";
    } else {
        // Generic command line
        return L"\"" + player_path + L"\" \"" + input_source + L"\"";
    }
}

bool RegisterCustomBufferClearEvent() {
    // Register a custom Windows message for buffer clearing
    static UINT buffer_clear_msg = 0;
    if (buffer_clear_msg == 0) {
        buffer_clear_msg = RegisterWindowMessageW(L"TARDSPLAYA_BUFFER_CLEAR");
    }
    return buffer_clear_msg != 0;
}

bool SendBufferClearMessage(HWND player_window) {
    if (!player_window || !IsWindow(player_window)) {
        return false;
    }
    
    static UINT buffer_clear_msg = RegisterWindowMessageW(L"TARDSPLAYA_BUFFER_CLEAR");
    if (buffer_clear_msg == 0) {
        return false;
    }
    
    // Send the custom message to the player window
    LRESULT result = SendMessageW(player_window, buffer_clear_msg, 0, 0);
    return result == 0; // Success if message was processed
}

} // namespace utils

// DirectShowMediaPlayer Implementation
DirectShowMediaPlayer::DirectShowMediaPlayer() 
    : ds_controller_(std::make_unique<DirectShowController>())
    , player_window_(nullptr)
    , directshow_enabled_(false) {
}

DirectShowMediaPlayer::~DirectShowMediaPlayer() {
    Stop();
}

bool DirectShowMediaPlayer::Initialize(const std::wstring& player_path,
                                     MediaEventCallback event_callback) {
    player_path_ = player_path;
    
    // Check if player supports DirectShow
    if (!DirectShowController::IsDirectShowCompatible(player_path)) {
        AddDebugLog(L"[DIRECTSHOW] Player not DirectShow compatible: " + player_path);
        return false; // Fall back to regular approach
    }
    
    AddDebugLog(L"[DIRECTSHOW] Initializing DirectShow buffer clear events for: " + player_path);
    
    // Initialize DirectShow controller for buffer clearing only
    if (!ds_controller_->Initialize(player_path, event_callback)) {
        AddDebugLog(L"[DIRECTSHOW] Failed to initialize DirectShow controller");
        return false;
    }
    
    directshow_enabled_ = true;
    AddDebugLog(L"[DIRECTSHOW] DirectShow buffer clear events initialized successfully");
    
    return true;
}

bool DirectShowMediaPlayer::HandleDiscontinuity() {
    if (!directshow_enabled_ || !ds_controller_->IsGraphReady()) {
        return false;
    }
    
    AddDebugLog(L"[DIRECTSHOW] Handling discontinuity with DirectShow events");
    
    // Method 1: Use DirectShow events to clear buffers
    bool ds_success = ds_controller_->ClearVideoBuffers();
    
    // Method 2: Send Windows message to player window
    bool msg_success = false;
    if (player_window_) {
        msg_success = utils::SendBufferClearMessage(player_window_);
    }
    
    // Method 3: Notify of segment transition
    bool notify_success = ds_controller_->NotifySegmentTransition();
    
    AddDebugLog(L"[DIRECTSHOW] Discontinuity handling complete - DS:" + 
               std::to_wstring(ds_success) + L" MSG:" + std::to_wstring(msg_success) + 
               L" NOTIFY:" + std::to_wstring(notify_success));
    
    return ds_success || msg_success || notify_success;
}

bool DirectShowMediaPlayer::IsPlayerHealthy() const {
    if (player_process_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD exit_code;
    if (!GetExitCodeProcess(player_process_, &exit_code)) {
        return false;
    }
    
    return exit_code == STILL_ACTIVE;
}

void DirectShowMediaPlayer::Stop() {
    if (directshow_enabled_) {
        ds_controller_.reset();
        directshow_enabled_ = false;
    }
    
    player_window_ = nullptr;
}

bool DirectShowMediaPlayer::FindPlayerWindow() {
    // Find player window by process name pattern matching
    player_window_ = nullptr;
    
    if (player_path_.find(L"mpc-hc") != std::wstring::npos) {
        player_window_ = FindWindow(L"MediaPlayerClassicW", nullptr);
        if (!player_window_) {
            player_window_ = FindWindow(L"MPC-HC", nullptr);
        }
    } else if (player_path_.find(L"vlc") != std::wstring::npos) {
        player_window_ = FindWindow(L"Qt5QWindowIcon", nullptr);
        if (!player_window_) {
            player_window_ = FindWindow(L"VLC media player", nullptr);
        }
    }
    
    return player_window_ != nullptr;
}

BOOL CALLBACK DirectShowMediaPlayer::FindWindowProc(HWND hwnd, LPARAM lParam) {
    // This method is no longer used since we find windows by class name
    return TRUE;
}
    }
    
    return TRUE; // Continue enumeration
}

} // namespace directshow_events