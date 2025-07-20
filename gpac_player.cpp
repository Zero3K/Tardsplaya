#include "gpac_player.h"
#include "gpac_minimal.h"
#include <iostream>
#include <sstream>

// Real GPAC structures using minimal implementation
struct GF_Terminal {
    GF_FilterSession* session;
    GF_Filter* ts_demux;
    std::unique_ptr<SimpleVideoRenderer> renderer;
    bool initialized;
    
    GF_Terminal() : session(nullptr), ts_demux(nullptr), initialized(false) {}
};

struct GF_User {
    void* opaque;
    std::function<void(const std::wstring&)> log_callback;
};

struct GF_Config {
    std::map<std::string, std::string> settings;
};

// GPAC Player Implementation
GpacPlayer::GpacPlayer()
    : m_terminal(nullptr)
    , m_user(nullptr) 
    , m_config(nullptr)
    , m_parentWindow(nullptr)
    , m_videoWindow(nullptr)
    , m_overlayWindow(nullptr)
    , m_initialized(false)
    , m_playing(false)
    , m_paused(false)
    , m_useSeparateWindow(false)
{
}

GpacPlayer::~GpacPlayer() {
    Stop();
    CleanupGpac();
}

bool GpacPlayer::Initialize(HWND parentWindow, const std::wstring& channelName) {
    if (m_initialized.load()) {
        LogMessage(L"GPAC player already initialized for channel: " + channelName);
        return true;
    }
    
    m_parentWindow = parentWindow;
    m_channelName = channelName;
    m_useSeparateWindow = (parentWindow == nullptr);
    
    LogMessage(L"Starting GPAC player initialization for channel: " + channelName + 
               (m_useSeparateWindow ? L" (separate window)" : L" (embedded window)"));
    
    // Initialize GPAC core
    if (!InitializeGpac()) {
        LogMessage(L"ERROR: Failed to initialize GPAC core for channel: " + channelName);
        return false;
    }
    LogMessage(L"GPAC core initialized successfully");
    
    // Create video window (separate or embedded based on mode)
    if (!CreateVideoWindow()) {
        LogMessage(L"ERROR: Failed to create video window for channel: " + channelName);
        CleanupGpac();
        return false;
    }
    LogMessage(L"Video window created successfully");
    
    // Create overlay window for ad-skipping messages
    if (!CreateOverlayWindow()) {
        LogMessage(L"ERROR: Failed to create overlay window for channel: " + channelName);
        CleanupGpac();
        return false;
    }
    LogMessage(L"Overlay window created successfully");
    
    m_initialized = true;
    LogMessage(L"GPAC player initialization completed successfully for channel: " + channelName);
    return true;
}

bool GpacPlayer::Play(const std::wstring& streamUrl) {
    if (!m_initialized.load()) {
        LogMessage(L"GPAC player not initialized");
        return false;
    }
    
    LogMessage(L"Starting playback of: " + streamUrl);
    m_currentUrl = streamUrl;
    
    // TODO: Real GPAC implementation would be:
    // gf_term_connect(m_terminal, streamUrl.c_str());
    
    // For now, simulate successful playback start
    m_playing = true;
    m_paused = false;
    
    // Show the video window
    if (m_videoWindow) {
        ShowWindow(m_videoWindow, SW_SHOW);
        UpdateWindow(m_videoWindow);
    }
    
    LogMessage(L"Playback started successfully");
    return true;
}

void GpacPlayer::Stop() {
    if (!m_playing.load()) {
        return;
    }
    
    LogMessage(L"Stopping playback");
    
    // TODO: Real GPAC implementation would be:
    // gf_term_disconnect(m_terminal);
    
    m_playing = false;
    m_paused = false;
    
    // Hide video window
    if (m_videoWindow) {
        ShowWindow(m_videoWindow, SW_HIDE);
    }
    
    // Hide overlay
    ShowAdSkippingMessage(false);
    
    LogMessage(L"Playback stopped");
}

void GpacPlayer::Pause() {
    if (!m_playing.load() || m_paused.load()) {
        return;
    }
    
    LogMessage(L"Pausing playback");
    
    // TODO: Real GPAC implementation would be:
    // gf_term_set_option(m_terminal, GF_OPT_PLAY_STATE, GF_STATE_PAUSED);
    
    m_paused = true;
}

void GpacPlayer::Resume() {
    if (!m_playing.load() || !m_paused.load()) {
        return;
    }
    
    LogMessage(L"Resuming playback");
    
    // TODO: Real GPAC implementation would be:
    // gf_term_set_option(m_terminal, GF_OPT_PLAY_STATE, GF_STATE_PLAYING);
    
    m_paused = false;
}

bool GpacPlayer::IsPlaying() const {
    return m_playing.load() && !m_paused.load();
}

HWND GpacPlayer::GetVideoWindow() const {
    return m_videoWindow;
}

void GpacPlayer::SetAdDetectionCallback(std::function<void(bool)> callback) {
    m_adDetectionCallback = callback;
}

void GpacPlayer::ShowAdSkippingMessage(bool show) {
    if (!m_overlayWindow) {
        return;
    }
    
    if (show) {
        ShowWindow(m_overlayWindow, SW_SHOW);
        UpdateWindow(m_overlayWindow);
        LogMessage(L"Showing 'Skipping ads' message");
    } else {
        ShowWindow(m_overlayWindow, SW_HIDE);
    }
}

void GpacPlayer::Resize(int width, int height) {
    if (m_videoWindow) {
        if (m_useSeparateWindow) {
            // For separate windows, resize the entire window
            RECT rect;
            GetWindowRect(m_videoWindow, &rect);
            SetWindowPos(m_videoWindow, nullptr, rect.left, rect.top, width, height, 
                        SWP_NOZORDER);
        } else {
            // For embedded windows, resize within parent
            SetWindowPos(m_videoWindow, nullptr, 0, 0, width, height, 
                        SWP_NOZORDER | SWP_NOMOVE);
        }
    }
    
    if (m_overlayWindow) {
        // Position overlay in top-right corner
        SetWindowPos(m_overlayWindow, HWND_TOP, width - 150, 10, 140, 30, 
                    SWP_SHOWWINDOW);
    }
}

void GpacPlayer::HandleDiscontinuity() {
    LogMessage(L"Handling stream discontinuity");
    
    // GPAC handles discontinuities much better than external players
    // TODO: Real implementation would use GPAC's discontinuity handling
    // For now, just log the event
    
    if (m_adDetectionCallback) {
        // Discontinuities often indicate ad boundaries
        m_adDetectionCallback(true);
    }
}

bool GpacPlayer::ProcessMpegTsData(const uint8_t* data, size_t dataSize) {
    if (!m_initialized.load() || !data || dataSize == 0) {
        LogMessage(L"Cannot process MPEG-TS data: player not initialized or invalid data");
        return false;
    }
    
    if (!m_terminal || !m_terminal->session || !m_terminal->ts_demux) {
        LogMessage(L"Cannot process MPEG-TS data: GPAC terminal not available");
        return false;
    }
    
    // Basic MPEG-TS packet validation (packets are 188 bytes)
    if (dataSize % 188 != 0) {
        LogMessage(L"Warning: MPEG-TS data size not aligned to 188-byte packets");
    }
    
    size_t numPackets = dataSize / 188;
    LogMessage(L"Processing " + std::to_wstring(numPackets) + L" MPEG-TS packets (" + 
               std::to_wstring(dataSize) + L" bytes)");
    
    // Feed data to GPAC minimal implementation
    GF_Err err = GpacMinimal::FeedTSData(m_terminal->ts_demux, data, dataSize);
    if (err != GF_OK) {
        LogMessage(L"GPAC failed to process MPEG-TS data, error: " + std::to_wstring(err));
        return false;
    }
    
    // Process the session to handle any decoded data
    err = GpacMinimal::ProcessSession(m_terminal->session);
    if (err != GF_OK) {
        LogMessage(L"GPAC failed to process session, error: " + std::to_wstring(err));
        return false;
    }
    
    // Check for video frames and render them
    if (m_terminal->renderer) {
        uint8_t* video_data;
        size_t video_size;
        uint32_t width, height;
        
        if (GpacMinimal::GetVideoFrame(m_terminal->session, &video_data, &video_size, &width, &height)) {
            m_terminal->renderer->RenderFrame(video_data, video_size, width, height);
            LogMessage(L"Rendered video frame: " + std::to_wstring(width) + L"x" + std::to_wstring(height));
        } else {
            // For demonstration, render a test pattern
            static int frame_counter = 0;
            if (++frame_counter % 100 == 0) { // Every 100 packets, show a test pattern
                uint8_t test_data[1920 * 1080 * 4]; // Dummy data
                m_terminal->renderer->RenderFrame(test_data, sizeof(test_data), 1920, 1080);
                LogMessage(L"Rendered test pattern frame");
            }
        }
    }
    
    LogMessage(L"MPEG-TS data processed successfully by GPAC minimal implementation");
    return true;
}

void GpacPlayer::SetLogCallback(std::function<void(const std::wstring&)> callback) {
    m_logCallback = callback;
}

bool GpacPlayer::InitializeGpac() {
    LogMessage(L"Initializing GPAC library with minimal implementation for MPEG-TS decoder support");
    
    try {
        // Initialize GPAC minimal system
        if (!GpacMinimal::Initialize()) {
            LogMessage(L"Failed to initialize GPAC minimal system");
            return false;
        }
        
        // Create GPAC structures
        m_config = new GF_Config();
        m_user = new GF_User();  
        m_terminal = new GF_Terminal();
        
        if (!m_config || !m_user || !m_terminal) {
            LogMessage(L"Failed to allocate GPAC structures");
            return false;
        }
        
        // Initialize user structure
        m_user->opaque = this;
        
        // Create filter session
        m_terminal->session = GpacMinimal::CreateSession();
        if (!m_terminal->session) {
            LogMessage(L"Failed to create GPAC filter session");
            return false;
        }
        
        // Create TS demux filter
        m_terminal->ts_demux = GpacMinimal::CreateTSDemuxFilter(m_terminal->session);
        if (!m_terminal->ts_demux) {
            LogMessage(L"Failed to create MPEG-TS demux filter");
            return false;
        }
        
        // Initialize video renderer if we have a video window
        if (m_videoWindow) {
            m_terminal->renderer = std::make_unique<SimpleVideoRenderer>();
            if (!m_terminal->renderer->Initialize(m_videoWindow, 800, 600)) {
                LogMessage(L"Warning: Failed to initialize video renderer, continuing without video output");
            } else {
                LogMessage(L"Video renderer initialized successfully");
            }
        }
        
        m_terminal->initialized = true;
        
        LogMessage(L"GPAC library initialized with minimal implementation - ready for MPEG-TS processing");
        LogMessage(L"Ready to process MPEG-TS streams with PAT/PMT parsing and basic video rendering");
        return true;
        
    } catch (const std::exception& e) {
        LogMessage(L"Exception during GPAC initialization: " + std::wstring(e.what(), e.what() + strlen(e.what())));
        return false;
    } catch (...) {
        LogMessage(L"Unknown exception during GPAC initialization");
        return false;
    }
}

void GpacPlayer::CleanupGpac() {
    LogMessage(L"Cleaning up GPAC resources");
    
    if (m_terminal) {
        if (m_terminal->renderer) {
            m_terminal->renderer->Shutdown();
            m_terminal->renderer.reset();
        }
        
        if (m_terminal->session) {
            GpacMinimal::DeleteSession(m_terminal->session);
            m_terminal->session = nullptr;
        }
        
        delete m_terminal;
        m_terminal = nullptr;
    }
    
    if (m_user) {
        delete m_user;
        m_user = nullptr;
    }
    
    if (m_config) {
        delete m_config;
        m_config = nullptr;
    }
    
    // Shutdown GPAC minimal system
    GpacMinimal::Shutdown();
    
    LogMessage(L"GPAC resources cleaned up");
}

bool GpacPlayer::CreateVideoWindow() {
    if (m_useSeparateWindow) {
        LogMessage(L"Creating separate video window for GPAC rendering");
        
        // Register a custom window class for video rendering if not already registered
        WNDCLASS wc = {};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"GpacVideoWindow";
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClass(&wc); // It's OK if this fails (already registered)
        
        // Create a separate top-level window for video rendering
        m_videoWindow = CreateWindowEx(
            WS_EX_APPWINDOW,
            L"GpacVideoWindow",
            (L"GPAC Video Player - " + m_channelName).c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,  // Separate window size
            nullptr,  // No parent
            nullptr,
            GetModuleHandle(nullptr),
            nullptr
        );
        
        if (!m_videoWindow) {
            DWORD error = GetLastError();
            LogMessage(L"Failed to create separate video window, error: " + std::to_wstring(error));
            return false;
        }
        
        LogMessage(L"Separate video window created successfully");
    } else {
        if (!m_parentWindow) {
            LogMessage(L"Cannot create embedded video window: no parent window provided");
            return false;
        }
        
        LogMessage(L"Creating embedded video window for GPAC rendering");
        
        // Create a child window for video rendering
        m_videoWindow = CreateWindowEx(
            0,
            L"STATIC",
            L"GPAC Video",
            WS_CHILD | WS_VISIBLE | WS_BORDER,
            0, 0, 400, 300,  // Initial size, will be resized
            m_parentWindow,
            nullptr,
            GetModuleHandle(nullptr),
            nullptr
        );
        
        if (!m_videoWindow) {
            DWORD error = GetLastError();
            LogMessage(L"Failed to create embedded video window, error: " + std::to_wstring(error));
            return false;
        }
        
        LogMessage(L"Embedded video window created successfully");
    }
    
    // Set background to black for video rendering
    SetClassLongPtr(m_videoWindow, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    
    // Set window title to show it's ready for MPEG-TS
    SetWindowText(m_videoWindow, (L"GPAC MPEG-TS Player - " + m_channelName).c_str());
    
    return true;
}

bool GpacPlayer::CreateOverlayWindow() {
    HWND parentForOverlay = m_useSeparateWindow ? m_videoWindow : m_parentWindow;
    
    if (!parentForOverlay) {
        LogMessage(L"Cannot create overlay window: no parent available");
        return false;
    }
    
    LogMessage(L"Creating overlay window for ad-skipping messages");
    
    // Create overlay window for "Skipping ads" message
    m_overlayWindow = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        L"STATIC",
        L"Skipping ads...",
        WS_CHILD | SS_CENTER,
        0, 0, 140, 30,
        parentForOverlay,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );
    
    if (!m_overlayWindow) {
        DWORD error = GetLastError();
        LogMessage(L"Failed to create overlay window, error: " + std::to_wstring(error));
        return false;
    }
    
    // Set overlay appearance
    if (!SetLayeredWindowAttributes(m_overlayWindow, RGB(0, 0, 0), 200, LWA_ALPHA)) {
        LogMessage(L"Warning: Failed to set overlay transparency");
    }
    
    // Set font and colors
    HFONT hFont = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
    if (hFont) {
        SendMessage(m_overlayWindow, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    
    // Initially hidden
    ShowWindow(m_overlayWindow, SW_HIDE);
    
    LogMessage(L"Overlay window created successfully");
    return true;
}

void GpacPlayer::UpdateOverlay() {
    if (m_overlayWindow) {
        InvalidateRect(m_overlayWindow, nullptr, TRUE);
        UpdateWindow(m_overlayWindow);
    }
}

void GpacPlayer::LogMessage(const std::wstring& message) {
    if (m_logCallback) {
        m_logCallback(L"[GPAC] " + message);
    }
}

void GpacPlayer::OnGpacLog(void* cbck, int level, int tool, const char* message) {
    // TODO: Handle GPAC log messages
    GpacPlayer* player = static_cast<GpacPlayer*>(cbck);
    if (player && message) {
        // Convert char* to wstring
        std::string str(message);
        std::wstring wstr(str.begin(), str.end());
        player->LogMessage(L"GPAC: " + wstr);
    }
}

void GpacPlayer::OnGpacEvent(void* user_data, int event_type, int error, const char* message) {
    // TODO: Handle GPAC events (discontinuities, ad detection, etc.)
    GpacPlayer* player = static_cast<GpacPlayer*>(user_data);
    if (player) {
        // Handle various GPAC events
        switch (event_type) {
            // TODO: Define event types based on actual GPAC API
            case 1: // Discontinuity detected
                player->HandleDiscontinuity();
                break;
            case 2: // Ad segment detected  
                player->ShowAdSkippingMessage(true);
                break;
            case 3: // Ad segment ended
                player->ShowAdSkippingMessage(false);
                break;
            default:
                break;
        }
    }
}

// Factory function
std::unique_ptr<GpacPlayer> CreateGpacPlayer() {
    return std::make_unique<GpacPlayer>();
}