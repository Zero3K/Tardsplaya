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
        LogMessage(std::wstring(L"GPAC player already initialized for channel: ") + channelName);
        return true;
    }
    
    m_parentWindow = parentWindow;
    m_channelName = channelName;
    m_useSeparateWindow = (parentWindow == nullptr);
    
    LogMessage(std::wstring(L"Starting GPAC player initialization for channel: ") + channelName + 
               (m_useSeparateWindow ? std::wstring(L" (separate window)") : std::wstring(L" (embedded window)")));
    
    // Initialize GPAC core
    LogMessage(L"Step 1: Initializing GPAC core...");
    if (!InitializeGpac()) {
        LogMessage(std::wstring(L"ERROR: Failed to initialize GPAC core for channel: ") + channelName);
        return false;
    }
    LogMessage(L"GPAC core initialized successfully");
    
    // Create video window (separate or embedded based on mode)
    LogMessage(L"Step 2: Creating video window...");
    if (!CreateVideoWindow()) {
        LogMessage(std::wstring(L"ERROR: Failed to create video window for channel: ") + channelName);
        CleanupGpac();
        return false;
    }
    LogMessage(L"Video window created successfully");
    
    // Update GPAC terminal with video window
    LogMessage(L"Step 3: Updating GPAC terminal with video window and audio...");
    if (m_terminal && m_videoWindow) {
        // Initialize video renderer
        m_terminal->renderer = std::make_unique<SimpleVideoRenderer>();
        if (!m_terminal->renderer->Initialize(m_videoWindow, 800, 600)) {
            LogMessage(L"Warning: Failed to initialize video renderer, but continuing");
        } else {
            LogMessage(L"Video renderer initialized successfully");
        }
        
        // Initialize audio renderer
        if (m_terminal->session->audio_renderer) {
            if (!m_terminal->session->audio_renderer->Initialize(m_videoWindow, 48000, 2)) {
                LogMessage(L"Warning: Failed to initialize audio renderer, but continuing");
            } else {
                LogMessage(L"Audio renderer initialized successfully");
            }
        }
        
        // Set up video and audio callbacks
        if (m_terminal->session->ts_parser) {
            LogMessage(L"Setting up video and audio callbacks...");
            m_terminal->session->ts_parser->SetVideoCallback(
                [this](const VideoFrame& frame) {
                    if (m_terminal->renderer) {
                        m_terminal->renderer->RenderFrame(frame);
                    }
                }
            );
            
            m_terminal->session->ts_parser->SetAudioCallback(
                [this](const AudioFrame& frame) {
                    // Process audio through the audio renderer
                    if (m_terminal->session->audio_renderer) {
                        m_terminal->session->audio_renderer->PlayAudioFrame(frame);
                    }
                    
                    static int audioCounter = 0;
                    if (++audioCounter % 50 == 0) { // Log every 50th audio frame to avoid spam
                        LogMessage(std::wstring(L"Audio frame #") + std::to_wstring(audioCounter / 50) + 
                                  std::wstring(L": ") + std::to_wstring(frame.sample_count) + std::wstring(L" samples, ") + 
                                  std::to_wstring(frame.sample_rate) + std::wstring(L"Hz, ") + std::to_wstring(frame.channels) + std::wstring(L" channels"));
                    }
                }
            );
            LogMessage(L"Video and audio callbacks set up successfully");
        }
    }
    
    // Create overlay window for ad-skipping messages
    LogMessage(L"Step 4: Creating overlay window...");
    if (!CreateOverlayWindow()) {
        LogMessage(std::wstring(L"ERROR: Failed to create overlay window for channel: ") + channelName);
        // Don't fail initialization just because overlay failed - overlay is optional
        LogMessage(L"Continuing without overlay window (optional feature)");
    } else {
        LogMessage(L"Overlay window created successfully");
    }
    
    m_initialized = true;
    LogMessage(std::wstring(L"GPAC player initialization completed successfully for channel: ") + channelName);
    return true;
}

bool GpacPlayer::Play(const std::wstring& streamUrl) {
    if (!m_initialized.load()) {
        LogMessage(L"GPAC player not initialized");
        return false;
    }
    
    LogMessage(std::wstring(L"Starting playback of: ") + streamUrl);
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
    
    // Only log periodically to avoid spam
    static int process_counter = 0;
    if (++process_counter % 100 == 0) {
        LogMessage(std::wstring(L"Processing ") + std::to_wstring(numPackets) + std::wstring(L" MPEG-TS packets (") + 
                   std::to_wstring(dataSize) + std::wstring(L" bytes) - batch #") + std::to_wstring(process_counter / 100));
    }
    
    // Feed data to GPAC minimal implementation
    GF_Err err = GpacMinimal::FeedTSData(m_terminal->ts_demux, data, dataSize);
    if (err != GF_OK) {
        LogMessage(std::wstring(L"GPAC failed to process MPEG-TS data, error: ") + std::to_wstring(err));
        return false;
    }
    
    // Process the session to handle any decoded data
    err = GpacMinimal::ProcessSession(m_terminal->session);
    if (err != GF_OK) {
        LogMessage(std::wstring(L"GPAC failed to process session, error: ") + std::to_wstring(err));
        return false;
    }
    
    // Force video rendering with the raw TS data to show activity
    if (m_terminal->renderer) {
        // Render more frequently to show stream activity
        static int render_counter = 0;
        if (++render_counter % 10 == 0) { // Every 10 data chunks
            m_terminal->renderer->RenderFrame(data, dataSize, 1920, 1080);
        }
    }
    
    // Periodic status reporting
    if (process_counter % 500 == 0) {
        LogMessage(std::wstring(L"MPEG-TS stream processing active - ") + std::to_wstring(process_counter * numPackets) + std::wstring(L" total packets processed"));
        
        if (m_terminal->session->ts_parser && m_terminal->session->ts_parser->IsInitialized()) {
            LogMessage(L"Stream parsing initialized - PAT/PMT processed, streams identified");
        }
    }
    
    return true;
}

void GpacPlayer::SetLogCallback(std::function<void(const std::wstring&)> callback) {
    m_logCallback = callback;
}

bool GpacPlayer::InitializeGpac() {
    LogMessage(L"Initializing GPAC library with minimal implementation for MPEG-TS decoder support");
    
    try {
        // Initialize GPAC minimal system
        LogMessage(L"Initializing GPAC minimal system...");
        if (!GpacMinimal::Initialize()) {
            LogMessage(L"Failed to initialize GPAC minimal system");
            return false;
        }
        LogMessage(L"GPAC minimal system initialized");
        
        // Create GPAC structures
        LogMessage(L"Creating GPAC structures...");
        m_config = new GF_Config();
        m_user = new GF_User();  
        m_terminal = new GF_Terminal();
        
        if (!m_config || !m_user || !m_terminal) {
            LogMessage(L"Failed to allocate GPAC structures");
            return false;
        }
        LogMessage(L"GPAC structures created");
        
        // Initialize user structure
        m_user->opaque = this;
        
        // Create filter session
        LogMessage(L"Creating GPAC filter session...");
        m_terminal->session = GpacMinimal::CreateSession();
        if (!m_terminal->session) {
            LogMessage(L"Failed to create GPAC filter session");
            return false;
        }
        LogMessage(L"GPAC filter session created");
        
        // Create TS demux filter
        LogMessage(L"Creating MPEG-TS demux filter...");
        m_terminal->ts_demux = GpacMinimal::CreateTSDemuxFilter(m_terminal->session);
        if (!m_terminal->ts_demux) {
            LogMessage(L"Failed to create MPEG-TS demux filter");
            return false;
        }
        LogMessage(L"MPEG-TS demux filter created");
        
        // Note: Video and audio renderers will be initialized after video window is created
        
        m_terminal->initialized = true;
        
        LogMessage(L"GPAC library initialized with minimal implementation - ready for MPEG-TS processing");
        LogMessage(L"Ready to process MPEG-TS streams with PAT/PMT parsing and basic video rendering");
        return true;
        
    } catch (const std::exception& e) {
        LogMessage(std::wstring(L"Exception during GPAC initialization: ") + std::wstring(e.what(), e.what() + strlen(e.what())));
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
            // Clean up audio renderer
            if (m_terminal->session->audio_renderer) {
                m_terminal->session->audio_renderer->Shutdown();
                m_terminal->session->audio_renderer.reset();
            }
            
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
            (std::wstring(L"GPAC Video Player - ") + m_channelName).c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,  // Separate window size
            nullptr,  // No parent
            nullptr,
            GetModuleHandle(nullptr),
            nullptr
        );
        
        if (!m_videoWindow) {
            DWORD error = GetLastError();
            LogMessage(std::wstring(L"Failed to create separate video window, error: ") + std::to_wstring(error));
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
            LogMessage(std::wstring(L"Failed to create embedded video window, error: ") + std::to_wstring(error));
            return false;
        }
        
        LogMessage(L"Embedded video window created successfully");
    }
    
    // Set background to black for video rendering
    SetClassLongPtr(m_videoWindow, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    
    // Set window title to show it's ready for MPEG-TS
    SetWindowText(m_videoWindow, (std::wstring(L"GPAC MPEG-TS Player - ") + m_channelName).c_str());
    
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
        LogMessage(std::wstring(L"Failed to create overlay window, error: ") + std::to_wstring(error));
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
        m_logCallback(std::wstring(L"[GPAC] ") + message);
    }
}

void GpacPlayer::OnGpacLog(void* cbck, int level, int tool, const char* message) {
    // TODO: Handle GPAC log messages
    GpacPlayer* player = static_cast<GpacPlayer*>(cbck);
    if (player && message) {
        // Convert char* to wstring
        std::string str(message);
        std::wstring wstr(str.begin(), str.end());
        player->LogMessage(std::wstring(L"GPAC: ") + wstr);
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