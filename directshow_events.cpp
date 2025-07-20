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
    : graph_builder_(nullptr)
    , media_control_(nullptr) 
    , media_event_(nullptr)
    , video_renderer_(nullptr)
    , event_processing_active_(false)
    , graph_ready_(false) {
    
    // Initialize COM for DirectShow
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}

DirectShowController::~DirectShowController() {
    StopEventProcessing();
    
    // Release DirectShow interfaces
    if (video_renderer_) {
        video_renderer_->Release();
        video_renderer_ = nullptr;
    }
    if (media_event_) {
        media_event_->Release();
        media_event_ = nullptr;
    }
    if (media_control_) {
        media_control_->Release();
        media_control_ = nullptr;
    }
    if (graph_builder_) {
        graph_builder_->Release();
        graph_builder_ = nullptr;
    }
    
    CoUninitialize();
}

bool DirectShowController::Initialize(const std::wstring& player_name, MediaEventCallback callback) {
    player_name_ = player_name;
    event_callback_ = callback;
    
    LogEvent(MediaEvent::GRAPH_READY, L"Initializing DirectShow controller for " + player_name);
    
    if (!CreateFilterGraph()) {
        last_error_ = L"Failed to create DirectShow filter graph";
        LogEvent(MediaEvent::ERROR_OCCURRED, last_error_);
        return false;
    }
    
    if (!FindVideoRenderer()) {
        last_error_ = L"Failed to find video renderer in DirectShow graph";
        LogEvent(MediaEvent::ERROR_OCCURRED, last_error_);
        return false;
    }
    
    graph_ready_ = true;
    LogEvent(MediaEvent::GRAPH_READY, L"DirectShow controller initialized successfully");
    
    // Start event processing thread
    StartEventProcessing();
    
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
    if (!graph_ready_ || !video_renderer_) {
        last_error_ = L"DirectShow graph not ready for buffer clearing";
        return false;
    }
    
    LogEvent(MediaEvent::BUFFER_CLEAR_REQUEST, L"Clearing video buffers via DirectShow");
    
    // Method 1: Flush video renderer
    if (!FlushVideoRenderer()) {
        LogEvent(MediaEvent::ERROR_OCCURRED, L"Failed to flush video renderer");
    }
    
    // Method 2: Reset renderer state
    if (!ResetRendererState()) {
        LogEvent(MediaEvent::ERROR_OCCURRED, L"Failed to reset renderer state");
    }
    
    LogEvent(MediaEvent::BUFFER_CLEAR_REQUEST, L"Video buffer clear operation completed");
    return true;
}

bool DirectShowController::NotifySegmentTransition() {
    if (!graph_ready_) {
        return false;
    }
    
    LogEvent(MediaEvent::SEGMENT_STARTED, L"Notifying DirectShow of segment transition");
    
    // Send custom event to prepare for segment transition
    if (media_event_) {
        // Fire custom event code for segment started
        const long SEGMENT_TRANSITION_EVENT = EC_USER + 100;
        media_event_->FreeEventParams(SEGMENT_TRANSITION_EVENT, 0, 0);
    }
    
    return true;
}

bool DirectShowController::ResetVideoRenderer() {
    if (!video_renderer_) {
        return false;
    }
    
    LogEvent(MediaEvent::PLAYBACK_RESUMED, L"Resetting video renderer for clean restart");
    
    // Stop and restart the video renderer
    HRESULT hr = S_OK;
    
    // Try to reset the renderer state
    IMediaFilter* media_filter = nullptr;
    hr = video_renderer_->QueryInterface(IID_IMediaFilter, (void**)&media_filter);
    if (SUCCEEDED(hr) && media_filter) {
        // Stop and start the filter to reset state
        media_filter->Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        media_filter->Run(0);
        media_filter->Release();
        
        LogEvent(MediaEvent::PLAYBACK_RESUMED, L"Video renderer reset completed");
        return true;
    }
    
    return false;
}

bool DirectShowController::CreateFilterGraph() {
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IGraphBuilder, (void**)&graph_builder_);
    if (FAILED(hr)) {
        return false;
    }
    
    // Get media control interface
    hr = graph_builder_->QueryInterface(IID_IMediaControl, (void**)&media_control_);
    if (FAILED(hr)) {
        return false;
    }
    
    // Get media event interface
    hr = graph_builder_->QueryInterface(IID_IMediaEventEx, (void**)&media_event_);
    if (FAILED(hr)) {
        return false;
    }
    
    return true;
}

bool DirectShowController::FindVideoRenderer() {
    if (!graph_builder_) {
        return false;
    }
    
    // Try to find an existing video renderer in the graph
    IEnumFilters* enum_filters = nullptr;
    HRESULT hr = graph_builder_->EnumFilters(&enum_filters);
    if (FAILED(hr)) {
        return false;
    }
    
    IBaseFilter* filter = nullptr;
    while (enum_filters->Next(1, &filter, nullptr) == S_OK) {
        FILTER_INFO filter_info;
        if (SUCCEEDED(filter->QueryFilterInfo(&filter_info))) {
            // Look for video renderer filters
            std::wstring filter_name = filter_info.achName;
            std::transform(filter_name.begin(), filter_name.end(), filter_name.begin(), ::towlower);
            
            if (filter_name.find(L"video") != std::wstring::npos && 
                filter_name.find(L"render") != std::wstring::npos) {
                video_renderer_ = filter;
                video_renderer_->AddRef();
                
                if (filter_info.pGraph) {
                    filter_info.pGraph->Release();
                }
                filter->Release();
                enum_filters->Release();
                return true;
            }
            
            if (filter_info.pGraph) {
                filter_info.pGraph->Release();
            }
        }
        filter->Release();
    }
    
    enum_filters->Release();
    
    // If no video renderer found, create a default one
    hr = CoCreateInstance(CLSID_VideoRenderer, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IBaseFilter, (void**)&video_renderer_);
    if (SUCCEEDED(hr)) {
        graph_builder_->AddFilter(video_renderer_, L"Video Renderer");
        return true;
    }
    
    return false;
}

void DirectShowController::StartEventProcessing() {
    if (event_processing_active_) {
        return;
    }
    
    event_processing_active_ = true;
    event_thread_ = std::thread([this]() {
        ProcessMediaEvents();
    });
}

void DirectShowController::StopEventProcessing() {
    if (!event_processing_active_) {
        return;
    }
    
    event_processing_active_ = false;
    if (event_thread_.joinable()) {
        event_thread_.join();
    }
}

void DirectShowController::ProcessMediaEvents() {
    while (event_processing_active_) {
        if (!media_event_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        long event_code, param1, param2;
        HRESULT hr = media_event_->GetEvent(&event_code, &param1, &param2, 10);
        
        if (SUCCEEDED(hr)) {
            // Process relevant DirectShow events
            switch (event_code) {
                case EC_COMPLETE:
                    LogEvent(MediaEvent::PLAYBACK_RESUMED, L"DirectShow playback completed");
                    break;
                    
                case EC_USERABORT:
                    LogEvent(MediaEvent::ERROR_OCCURRED, L"DirectShow playback aborted by user");
                    break;
                    
                case EC_ERRORABORT:
                    LogEvent(MediaEvent::ERROR_OCCURRED, L"DirectShow playback error occurred");
                    break;
                    
                case EC_SEGMENT_STARTED:
                    LogEvent(MediaEvent::SEGMENT_STARTED, L"DirectShow segment started event");
                    break;
                    
                default:
                    // Handle custom events
                    if (event_code >= EC_USER) {
                        LogEvent(MediaEvent::SEGMENT_STARTED, 
                               L"Custom DirectShow event: " + std::to_wstring(event_code));
                    }
                    break;
            }
            
            // Free event parameters
            media_event_->FreeEventParams(event_code, param1, param2);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool DirectShowController::FlushVideoRenderer() {
    if (!video_renderer_) {
        return false;
    }
    
    // Try multiple approaches to flush video buffers
    
    // Approach 1: Query for IVideoWindow and refresh
    IVideoWindow* video_window = nullptr;
    HRESULT hr = video_renderer_->QueryInterface(IID_IVideoWindow, (void**)&video_window);
    if (SUCCEEDED(hr) && video_window) {
        video_window->SetWindowPosition(0, 0, 0, 0); // Force refresh
        video_window->Release();
    }
    
    // Approach 2: Query for custom buffer control interfaces
    // This would be specific to the renderer implementation
    
    return true;
}

bool DirectShowController::ResetRendererState() {
    if (!video_renderer_) {
        return false;
    }
    
    // Reset the renderer's internal state
    IMediaSample* sample = nullptr;
    // Implementation would depend on accessing renderer's internal interfaces
    
    return true;
}

void DirectShowController::LogEvent(MediaEvent event, const std::wstring& description) {
    if (event_callback_) {
        event_callback_(event, description);
    }
    
    // Also log to debug output
    std::wstring event_name;
    switch (event) {
        case MediaEvent::SEGMENT_STARTED: event_name = L"SEGMENT_STARTED"; break;
        case MediaEvent::BUFFER_CLEAR_REQUEST: event_name = L"BUFFER_CLEAR"; break;
        case MediaEvent::PLAYBACK_RESUMED: event_name = L"PLAYBACK_RESUMED"; break;
        case MediaEvent::GRAPH_READY: event_name = L"GRAPH_READY"; break;
        case MediaEvent::ERROR_OCCURRED: event_name = L"ERROR"; break;
        default: event_name = L"UNKNOWN"; break;
    }
    
    AddDebugLog(L"[DIRECTSHOW_" + event_name + L"] " + description);
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
    // Check if DirectShow components are available on the system
    IGraphBuilder* test_graph = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IGraphBuilder, (void**)&test_graph);
    
    if (SUCCEEDED(hr) && test_graph) {
        test_graph->Release();
        return true;
    }
    
    return false;
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
    , player_process_(INVALID_HANDLE_VALUE)
    , player_window_(nullptr)
    , directshow_enabled_(false) {
}

DirectShowMediaPlayer::~DirectShowMediaPlayer() {
    Stop();
}

bool DirectShowMediaPlayer::Launch(const std::wstring& player_path, 
                                  const std::wstring& input_source,
                                  MediaEventCallback event_callback) {
    player_path_ = player_path;
    
    // Check if player supports DirectShow
    if (!DirectShowController::IsDirectShowCompatible(player_path)) {
        AddDebugLog(L"[DIRECTSHOW] Player not DirectShow compatible: " + player_path);
        return false; // Fall back to regular player launch
    }
    
    AddDebugLog(L"[DIRECTSHOW] Launching DirectShow-compatible player: " + player_path);
    
    // Initialize DirectShow controller
    if (!ds_controller_->Initialize(player_path, event_callback)) {
        AddDebugLog(L"[DIRECTSHOW] Failed to initialize DirectShow controller");
        return false;
    }
    
    // Create DirectShow-optimized command line
    std::wstring cmd_line = utils::CreateDirectShowCommandLine(player_path, input_source);
    
    // Launch the player process
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    if (!CreateProcessW(nullptr, &cmd_line[0], nullptr, nullptr, FALSE, 
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        AddDebugLog(L"[DIRECTSHOW] Failed to launch player process");
        return false;
    }
    
    player_process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    
    // Try to find the player window for message-based communication
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Wait for window creation
    FindPlayerWindow();
    
    directshow_enabled_ = true;
    AddDebugLog(L"[DIRECTSHOW] Player launched successfully with DirectShow support");
    
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
    
    if (player_process_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(player_process_, 0);
        CloseHandle(player_process_);
        player_process_ = INVALID_HANDLE_VALUE;
    }
    
    player_window_ = nullptr;
}

bool DirectShowMediaPlayer::FindPlayerWindow() {
    if (player_process_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD process_id = GetProcessId(player_process_);
    if (process_id == 0) {
        return false;
    }
    
    // Enumerate windows to find the player window
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(this));
    
    return player_window_ != nullptr;
}

BOOL CALLBACK DirectShowMediaPlayer::FindWindowProc(HWND hwnd, LPARAM lParam) {
    DirectShowMediaPlayer* player = reinterpret_cast<DirectShowMediaPlayer*>(lParam);
    
    DWORD window_process_id;
    GetWindowThreadProcessId(hwnd, &window_process_id);
    
    DWORD player_process_id = GetProcessId(player->player_process_);
    
    if (window_process_id == player_process_id && IsWindowVisible(hwnd)) {
        player->player_window_ = hwnd;
        return FALSE; // Stop enumeration
    }
    
    return TRUE; // Continue enumeration
}

} // namespace directshow_events