#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX      // Prevent Windows min/max macros
#endif
#include <windows.h>
#include "builtin_player.h"
#include "stream_thread.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>

// SimpleBuiltinPlayer implementation
SimpleBuiltinPlayer::SimpleBuiltinPlayer()
    : m_hwndStatus(nullptr)
    , m_hwndVideo(nullptr)
    , m_isPlaying(false)
    , m_isInitialized(false)
    , m_processRunning(false)
    , m_totalBytesProcessed(0)
    , m_segmentsProcessed(0)
    , m_pSession(nullptr)
    , m_pVideoDisplay(nullptr)
    , m_pByteStream(nullptr)
    , m_pSourceResolver(nullptr)
    , m_pMediaSource(nullptr)
    , m_pTopology(nullptr)
    , m_mfInitialized(false)
{
}

SimpleBuiltinPlayer::~SimpleBuiltinPlayer() {
    Cleanup();
    CleanupMediaFoundation();
}

bool SimpleBuiltinPlayer::Initialize(HWND hwndStatus) {
    AddDebugLog(L"[SIMPLE_PLAYER] Initializing simple built-in player");
    
    if (m_isInitialized.load()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Player already initialized");
        return true;
    }
    
    // Initialize Media Foundation
    if (!InitializeMediaFoundation()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to initialize Media Foundation");
        return false;
    }
    
    m_hwndStatus = hwndStatus;
    m_isInitialized = true;
    
    // Update initial status
    UpdateStatus();
    
    AddDebugLog(L"[SIMPLE_PLAYER] Simple built-in player initialized successfully");
    return true;
}

bool SimpleBuiltinPlayer::StartStream(const std::wstring& streamName, const std::wstring& quality) {
    AddDebugLog(L"[SIMPLE_PLAYER] Starting stream: " + streamName + L" (Quality: " + quality + L")");
    
    if (!m_isInitialized.load()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Player not initialized");
        return false;
    }
    
    if (m_isPlaying.load()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Already playing, stopping current stream");
        StopStream();
    }
    
    m_currentStreamName = streamName;
    m_currentQuality = quality;
    m_totalBytesProcessed = 0;
    m_segmentsProcessed = 0;
    
    // Create separate video window based on quality
    if (!CreateVideoWindow(quality)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create video window");
        return false;
    }
    
    // Configure video rendering for the window
    if (!SetVideoWindow(m_hwndVideo)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to configure video window");
        return false;
    }
    
    // Start the processing thread
    m_processRunning = true;
    m_processThread = std::thread(&SimpleBuiltinPlayer::ProcessThreadProc, this);
    
    m_isPlaying = true;
    
    // Video playback will start automatically when we have enough data
    
    UpdateStatus();
    
    // Trigger initial repaint to show streaming state
    if (m_hwndVideo) {
        InvalidateRect(m_hwndVideo, NULL, TRUE);
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] Stream started successfully: " + streamName);
    return true;
}

void SimpleBuiltinPlayer::StopStream() {
    AddDebugLog(L"[SIMPLE_PLAYER] Stopping stream");
    
    m_isPlaying = false;
    m_processRunning = false;
    
    // Stop the processing thread
    if (m_processThread.joinable()) {
        m_processThread.join();
    }
    
    // Clear data buffer
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        while (!m_dataBuffer.empty()) {
            m_dataBuffer.pop();
        }
    }
    
    // Destroy the separate video window
    DestroyVideoWindow();
    
    UpdateStatus();
    AddDebugLog(L"[SIMPLE_PLAYER] Stream stopped");
}

bool SimpleBuiltinPlayer::FeedData(const char* data, size_t size) {
    if (!m_isPlaying.load() || !data || size == 0) {
        return false;
    }
    
    // Add data to buffer for processing
    std::vector<char> dataVec(data, data + size);
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_dataBuffer.push(std::move(dataVec));
        
        // Limit buffer size to prevent memory bloat
        while (m_dataBuffer.size() > 50) {
            m_dataBuffer.pop();
        }
    }
    
    return true;
}

bool SimpleBuiltinPlayer::IsPlaying() const {
    return m_isPlaying.load();
}

size_t SimpleBuiltinPlayer::GetTotalBytesProcessed() const {
    return m_totalBytesProcessed.load();
}

size_t SimpleBuiltinPlayer::GetSegmentsProcessed() const {
    return m_segmentsProcessed.load();
}

HWND SimpleBuiltinPlayer::GetVideoWindow() const {
    return m_hwndVideo;
}

void SimpleBuiltinPlayer::UpdateStatus() {
    if (!m_hwndStatus) return;
    
    std::wstring statusText = FormatStatus();
    SetWindowTextW(m_hwndStatus, statusText.c_str());
}

void SimpleBuiltinPlayer::Cleanup() {
    AddDebugLog(L"[SIMPLE_PLAYER] Cleaning up simple built-in player");
    
    StopStream();
    DestroyVideoWindow();
    CleanupMediaFoundation();
    m_isInitialized = false;
}

void SimpleBuiltinPlayer::ProcessThreadProc() {
    AddDebugLog(L"[SIMPLE_PLAYER] Processing thread started");
    
    while (m_processRunning.load()) {
        std::vector<char> data;
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            if (!m_dataBuffer.empty()) {
                data = std::move(m_dataBuffer.front());
                m_dataBuffer.pop();
            }
        }
        
        if (!data.empty()) {
            ProcessSegment(data);
            
            // Update status periodically (every 10 segments)
            if (m_segmentsProcessed.load() % 10 == 0) {
                UpdateStatus();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] Processing thread ended");
}

void SimpleBuiltinPlayer::ProcessSegment(const std::vector<char>& data) {
    // Process the actual MPEG-TS segment data for video playback
    size_t segmentSize = data.size();
    m_totalBytesProcessed += segmentSize;
    m_segmentsProcessed++;
    
    if (!data.empty()) {
        // Write stream data to a temporary file for playback
        static std::wstring temp_file_path;
        static HANDLE temp_file_handle = INVALID_HANDLE_VALUE;
        static bool playback_started = false;
        
        // Create temporary file on first segment
        if (temp_file_handle == INVALID_HANDLE_VALUE) {
            wchar_t temp_path[MAX_PATH];
            wchar_t temp_filename[MAX_PATH];
            
            GetTempPath(MAX_PATH, temp_path);
            GetTempFileName(temp_path, L"TDS", 0, temp_filename);
            
            // Change extension to .ts for better codec detection
            temp_file_path = temp_filename;
            size_t dot_pos = temp_file_path.find_last_of(L'.');
            if (dot_pos != std::wstring::npos) {
                temp_file_path = temp_file_path.substr(0, dot_pos) + L".ts";
            }
            
            temp_file_handle = CreateFile(
                temp_file_path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr
            );
            
            if (temp_file_handle != INVALID_HANDLE_VALUE) {
                AddDebugLog(L"[SIMPLE_PLAYER] Created temporary stream file: " + temp_file_path);
            } else {
                AddDebugLog(L"[SIMPLE_PLAYER] Failed to create temporary file");
                return;
            }
        }
        
        // Write segment data to file
        if (temp_file_handle != INVALID_HANDLE_VALUE) {
            DWORD bytes_written = 0;
            WriteFile(temp_file_handle, data.data(), (DWORD)segmentSize, &bytes_written, nullptr);
            FlushFileBuffers(temp_file_handle);
            
            // Try to start playback after we have enough data
            if (!playback_started && m_totalBytesProcessed >= 2 * 1024 * 1024) { // 2MB threshold
                AddDebugLog(L"[SIMPLE_PLAYER] Attempting to start video playback with " + 
                           std::to_wstring(m_totalBytesProcessed / 1024) + L" KB of data");
                
                if (StartVideoPlaybackFromFile(temp_file_path)) {
                    playback_started = true;
                    AddDebugLog(L"[SIMPLE_PLAYER] Video playback started successfully");
                } else {
                    AddDebugLog(L"[SIMPLE_PLAYER] Failed to start video playback");
                }
            }
        }
        
        AddDebugLog(L"[SIMPLE_PLAYER] Processed segment " + std::to_wstring(m_segmentsProcessed.load()) + 
                   L", size=" + std::to_wstring(segmentSize) + L" bytes, total=" + 
                   std::to_wstring(m_totalBytesProcessed / 1024) + L" KB");
    }
    
    // Update video window display every few segments
    if (m_hwndVideo && (m_segmentsProcessed.load() % 5 == 0)) {
        InvalidateRect(m_hwndVideo, NULL, FALSE);
    }
    
    // Log occasional debug info
    if (m_segmentsProcessed.load() % 20 == 0) {
        AddDebugLog(L"[SIMPLE_PLAYER] Processed " + std::to_wstring(m_segmentsProcessed.load()) + 
                   L" segments, " + std::to_wstring(m_totalBytesProcessed.load() / 1024) + L" KB total");
    }
}

std::wstring SimpleBuiltinPlayer::FormatStatus() const {
    if (!m_isPlaying.load()) {
        return L"Built-in Player: Stopped";
    }
    
    size_t totalKB = m_totalBytesProcessed.load() / 1024;
    size_t segments = m_segmentsProcessed.load();
    
    std::wstringstream ss;
    ss << L"Built-in Player: " << m_currentStreamName 
       << L" | " << segments << L" segments"
       << L" | " << totalKB << L" KB processed";
    
    return ss.str();
}

bool SimpleBuiltinPlayer::CreateVideoWindow(const std::wstring& quality) {
    // Parse quality to determine window size
    auto size = ParseQualitySize(quality);
    int width = size.first;
    int height = size.second;
    
    // Add window decorations and ensure minimum size
    int windowWidth = width + 16;  // Account for borders
    int windowHeight = height + 39; // Account for title bar and borders
    
    // Ensure minimum size
    if (windowWidth < 320) windowWidth = 320;
    if (windowHeight < 240) windowHeight = 240;
    
    // Center the window on screen
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - windowWidth) / 2;
    int y = (screenHeight - windowHeight) / 2;
    
    // Create window class if not already registered
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = VideoWindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"TardsplayaVideoWindow";
        
        if (RegisterClassEx(&wc)) {
            classRegistered = true;
        }
    }
    
    // Create the video window
    std::wstring title = L"Tardsplaya - " + m_currentStreamName + L" (" + quality + L")";
    m_hwndVideo = CreateWindowEx(
        0,
        L"TardsplayaVideoWindow",
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        x, y, windowWidth, windowHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );
    
    if (!m_hwndVideo) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create video window");
        return false;
    }
    
    // Store the player instance in window user data for message handling
    SetWindowLongPtr(m_hwndVideo, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    
    // Show the window
    ShowWindow(m_hwndVideo, SW_SHOW);
    UpdateWindow(m_hwndVideo);
    
    AddDebugLog(L"[SIMPLE_PLAYER] Created video window (" + std::to_wstring(width) + L"x" + std::to_wstring(height) + L")");
    return true;
}

void SimpleBuiltinPlayer::DestroyVideoWindow() {
    if (m_hwndVideo) {
        DestroyWindow(m_hwndVideo);
        m_hwndVideo = nullptr;
        AddDebugLog(L"[SIMPLE_PLAYER] Video window destroyed");
    }
}

std::pair<int, int> SimpleBuiltinPlayer::ParseQualitySize(const std::wstring& quality) {
    // Default size
    int defaultWidth = 854;
    int defaultHeight = 480;
    
    if (quality.empty()) {
        return { defaultWidth, defaultHeight };
    }
    
    // Look for resolution patterns like "1920x1080", "1280x720", etc.
    size_t xPos = quality.find(L'x');
    if (xPos != std::wstring::npos) {
        try {
            std::wstring widthStr = quality.substr(0, xPos);
            std::wstring heightStr = quality.substr(xPos + 1);
            
            // Remove any non-digit characters
            widthStr.erase(std::remove_if(widthStr.begin(), widthStr.end(), [](wchar_t c) { return !std::isdigit(c); }), widthStr.end());
            heightStr.erase(std::remove_if(heightStr.begin(), heightStr.end(), [](wchar_t c) { return !std::isdigit(c); }), heightStr.end());
            
            if (!widthStr.empty() && !heightStr.empty()) {
                int width = std::stoi(widthStr);
                int height = std::stoi(heightStr);
                
                if (width > 0 && height > 0 && width <= 3840 && height <= 2160) {
                    return { width, height };
                }
            }
        } catch (...) {
            // Fall through to default handling
        }
    }
    
    // Handle common quality names
    if (quality.find(L"1080") != std::wstring::npos) {
        return { 1920, 1080 };
    } else if (quality.find(L"720") != std::wstring::npos) {
        return { 1280, 720 };
    } else if (quality.find(L"480") != std::wstring::npos) {
        return { 854, 480 };
    } else if (quality.find(L"360") != std::wstring::npos) {
        return { 640, 360 };
    } else if (quality.find(L"source") != std::wstring::npos || quality.find(L"best") != std::wstring::npos) {
        return { 1920, 1080 }; // Assume source is 1080p
    }
    
    return { defaultWidth, defaultHeight };
}

LRESULT CALLBACK SimpleBuiltinPlayer::VideoWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SimpleBuiltinPlayer* player = reinterpret_cast<SimpleBuiltinPlayer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Fill with black background
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            
            // Show video content or status
            if (player && player->IsPlaying()) {
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkColor(hdc, RGB(0, 0, 0));
                
                // Show streaming visual indicator
                size_t segments = player->GetSegmentsProcessed();
                size_t bytes = player->GetTotalBytesProcessed();
                
                // Create a visual representation of the stream
                int centerX = rect.right / 2;
                int centerY = rect.bottom / 2;
                
                // Draw animated indicators based on streaming data
                HBRUSH greenBrush = CreateSolidBrush(RGB(0, 255, 0));
                HBRUSH blueBrush = CreateSolidBrush(RGB(0, 100, 255));
                HBRUSH redBrush = CreateSolidBrush(RGB(255, 100, 100));
                
                // Draw animated bars representing stream activity
                int barCount = 20;
                int barWidth = rect.right / (barCount + 2);
                int maxBarHeight = rect.bottom / 3;
                
                for (int i = 0; i < barCount; i++) {
                    // Calculate bar height based on stream activity
                    int height = (segments % (i + 5)) * maxBarHeight / 20;
                    height = std::max(5, height);
                    
                    RECT barRect;
                    barRect.left = (i + 1) * barWidth;
                    barRect.right = barRect.left + barWidth - 2;
                    barRect.bottom = centerY + maxBarHeight / 2;
                    barRect.top = barRect.bottom - height;
                    
                    // Color based on activity level
                    HBRUSH brush = (height > maxBarHeight / 2) ? greenBrush : 
                                  (height > maxBarHeight / 4) ? blueBrush : redBrush;
                    FillRect(hdc, &barRect, brush);
                }
                
                // Clean up brushes
                DeleteObject(greenBrush);
                DeleteObject(blueBrush);
                DeleteObject(redBrush);
                
                // Stream info text
                std::wstring text = L"🔴 LIVE STREAM PLAYING\n\n";
                text += L"Channel: " + player->m_currentStreamName + L"\n";
                text += L"Quality: " + player->m_currentQuality + L"\n";
                text += L"Segments: " + std::to_wstring(segments) + L"\n";
                text += L"Data: " + std::to_wstring(bytes / 1024) + L" KB\n\n";
                text += L"Stream is active and data is being processed...";
                
                // Draw text at the top
                RECT textRect = rect;
                textRect.bottom = centerY - maxBarHeight;
                DrawText(hdc, text.c_str(), -1, &textRect, DT_CENTER | DT_TOP | DT_WORDBREAK);
                
            } else {
                SetTextColor(hdc, RGB(128, 128, 128));
                SetBkColor(hdc, RGB(0, 0, 0));
                
                // Draw loading animation
                std::wstring loadText = L"Starting video stream...";
                DrawText(hdc, loadText.c_str(), -1, &rect, DT_CENTER | DT_VCENTER);
                
                // Draw loading spinner
                int centerX = rect.right / 2;
                int centerY = rect.bottom / 2 + 40;
                int radius = 20;
                
                HPEN pen = CreatePen(PS_SOLID, 3, RGB(100, 100, 100));
                HPEN oldPen = (HPEN)SelectObject(hdc, pen);
                
                static DWORD startTime = GetTickCount();
                DWORD elapsed = GetTickCount() - startTime;
                double angle = (elapsed % 2000) * 3.14159 * 2 / 2000; // 2 second rotation
                
                int x1 = centerX + (int)(std::cos(angle) * radius);
                int y1 = centerY + (int)(std::sin(angle) * radius);
                int x2 = centerX + (int)(std::cos(angle + 3.14159) * radius);
                int y2 = centerY + (int)(std::sin(angle + 3.14159) * radius);
                
                MoveToEx(hdc, x1, y1, NULL);
                LineTo(hdc, x2, y2);
                
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
                
                // Trigger repaint for animation
                InvalidateRect(hwnd, NULL, FALSE);
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_CLOSE:
            // Stop the stream when window is closed
            if (player) {
                player->StopStream();
            }
            return 0;
            
        case WM_DESTROY:
            // Clear the user data
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Media Foundation implementation
bool SimpleBuiltinPlayer::InitializeMediaFoundation() {
    AddDebugLog(L"[SIMPLE_PLAYER] Initializing Media Foundation");
    
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to start Media Foundation, hr=" + std::to_wstring(hr));
        return false;
    }
    
    m_mfInitialized = true;
    AddDebugLog(L"[SIMPLE_PLAYER] Media Foundation initialized successfully");
    return true;
}

void SimpleBuiltinPlayer::CleanupMediaFoundation() {
    if (!m_mfInitialized.load()) return;
    
    AddDebugLog(L"[SIMPLE_PLAYER] Cleaning up Media Foundation");
    
    // Release all Media Foundation objects
    if (m_pVideoDisplay) {
        m_pVideoDisplay->Release();
        m_pVideoDisplay = nullptr;
    }
    if (m_pSession) {
        m_pSession->Close();
        m_pSession->Release();
        m_pSession = nullptr;
    }
    if (m_pTopology) {
        m_pTopology->Release();
        m_pTopology = nullptr;
    }
    if (m_pMediaSource) {
        m_pMediaSource->Release();
        m_pMediaSource = nullptr;
    }
    if (m_pSourceResolver) {
        m_pSourceResolver->Release();
        m_pSourceResolver = nullptr;
    }
    if (m_pByteStream) {
        m_pByteStream->Release();
        m_pByteStream = nullptr;
    }
    
    MFShutdown();
    m_mfInitialized = false;
    AddDebugLog(L"[SIMPLE_PLAYER] Media Foundation cleanup complete");
}

bool SimpleBuiltinPlayer::CreateVideoSession() {
    AddDebugLog(L"[SIMPLE_PLAYER] Creating Media Foundation video session");
    
    HRESULT hr = S_OK;
    
    // Create source resolver
    hr = MFCreateSourceResolver(&m_pSourceResolver);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create source resolver, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Create a byte stream for MPEG-TS data
    hr = MFCreateTempFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, &m_pByteStream);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create byte stream, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Create media session
    hr = MFCreateMediaSession(NULL, &m_pSession);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create media session, hr=" + std::to_wstring(hr));
        return false;
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] Media Foundation video session created successfully");
    return true;
}

bool SimpleBuiltinPlayer::SetVideoWindow(HWND hwnd) {
    // Store the video window handle for future use
    // In a full implementation, this would configure the video renderer
    AddDebugLog(L"[SIMPLE_PLAYER] Video window configured");
    return true;
}

bool SimpleBuiltinPlayer::CreateMediaSourceFromData(const std::vector<char>& data) {
    AddDebugLog(L"[SIMPLE_PLAYER] Creating media source from " + std::to_wstring(data.size()) + L" bytes of data");
    
    HRESULT hr = S_OK;
    
    if (!m_pSourceResolver) {
        hr = MFCreateSourceResolver(&m_pSourceResolver);
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to create source resolver, hr=" + std::to_wstring(hr));
            return false;
        }
    }
    
    // Create a memory-backed byte stream from the accumulated data
    if (m_pByteStream) {
        m_pByteStream->Release();
        m_pByteStream = nullptr;
    }
    
    hr = MFCreateMFByteStreamOnStream(nullptr, &m_pByteStream);
    if (FAILED(hr)) {
        // Try alternative approach with temp file
        hr = MFCreateTempFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, &m_pByteStream);
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to create byte stream, hr=" + std::to_wstring(hr));
            return false;
        }
    }
    
    // Write initial data to the byte stream
    ULONG bytesWritten = 0;
    hr = m_pByteStream->Write(reinterpret_cast<const BYTE*>(data.data()), (ULONG)data.size(), &bytesWritten);
    if (FAILED(hr) || bytesWritten != data.size()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to write initial data to byte stream, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Set the current position back to the beginning
    hr = m_pByteStream->SetCurrentPosition(0);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to seek to beginning of byte stream, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Try to create a media source from the byte stream
    DWORD dwFlags = 0;
    MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;
    IUnknown* pSource = nullptr;
    
    hr = m_pSourceResolver->CreateObjectFromByteStream(
        m_pByteStream,
        nullptr,
        MF_RESOLUTION_MEDIASOURCE,
        nullptr,
        &ObjectType,
        &pSource
    );
    
    if (SUCCEEDED(hr) && pSource) {
        hr = pSource->QueryInterface(IID_PPV_ARGS(&m_pMediaSource));
        pSource->Release();
        
        if (SUCCEEDED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Successfully created media source from data");
            return true;
        }
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] Failed to create media source from data, hr=" + std::to_wstring(hr));
    return false;
}

bool SimpleBuiltinPlayer::CreateTopology() {
    AddDebugLog(L"[SIMPLE_PLAYER] Creating topology for media playback");
    
    if (!m_pMediaSource) {
        AddDebugLog(L"[SIMPLE_PLAYER] No media source available for topology creation");
        return false;
    }
    
    HRESULT hr = S_OK;
    
    // Create the topology
    hr = MFCreateTopology(&m_pTopology);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create topology, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Get presentation descriptor from media source
    IMFPresentationDescriptor* pPD = nullptr;
    hr = m_pMediaSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create presentation descriptor, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Get the number of streams
    DWORD cStreams = 0;
    hr = pPD->GetStreamDescriptorCount(&cStreams);
    if (FAILED(hr)) {
        pPD->Release();
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to get stream count, hr=" + std::to_wstring(hr));
        return false;
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] Found " + std::to_wstring(cStreams) + L" streams in media source");
    
    // Add topology nodes for each stream
    for (DWORD i = 0; i < cStreams; i++) {
        BOOL fSelected = FALSE;
        IMFStreamDescriptor* pSD = nullptr;
        
        hr = pPD->GetStreamDescriptorByIndex(i, &fSelected, &pSD);
        if (SUCCEEDED(hr) && fSelected) {
            // Create source node
            IMFTopologyNode* pSourceNode = nullptr;
            hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSourceNode);
            if (SUCCEEDED(hr)) {
                // Set source node attributes
                pSourceNode->SetUnknown(MF_TOPONODE_SOURCE, m_pMediaSource);
                pSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD);
                pSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);
                
                // Add source node to topology
                m_pTopology->AddNode(pSourceNode);
                
                // Create output node for video renderer
                IMFTopologyNode* pOutputNode = nullptr;
                hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutputNode);
                if (SUCCEEDED(hr)) {
                    // Create video renderer
                    IMFActivate* pRendererActivate = nullptr;
                    hr = MFCreateVideoRendererActivate(m_hwndVideo, &pRendererActivate);
                    if (SUCCEEDED(hr)) {
                        pOutputNode->SetObject(pRendererActivate);
                        m_pTopology->AddNode(pOutputNode);
                        
                        // Connect source to output
                        pSourceNode->ConnectOutput(0, pOutputNode, 0);
                        
                        AddDebugLog(L"[SIMPLE_PLAYER] Successfully created topology for stream " + std::to_wstring(i));
                        pRendererActivate->Release();
                    }
                    pOutputNode->Release();
                }
                pSourceNode->Release();
            }
        }
        
        if (pSD) pSD->Release();
    }
    
    pPD->Release();
    
    AddDebugLog(L"[SIMPLE_PLAYER] Topology creation completed");
    return true;
}

bool SimpleBuiltinPlayer::StartVideoPlaybackFromFile(const std::wstring& filePath) {
    AddDebugLog(L"[SIMPLE_PLAYER] Starting video playback from file: " + filePath);
    
    HRESULT hr = S_OK;
    
    // Create source resolver if not already created
    if (!m_pSourceResolver) {
        hr = MFCreateSourceResolver(&m_pSourceResolver);
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to create source resolver, hr=" + std::to_wstring(hr));
            return false;
        }
    }
    
    // Create media source from file
    MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;
    IUnknown* pSource = nullptr;
    
    hr = m_pSourceResolver->CreateObjectFromURL(
        filePath.c_str(),
        MF_RESOLUTION_MEDIASOURCE,
        nullptr,
        &ObjectType,
        &pSource
    );
    
    if (FAILED(hr) || !pSource) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create media source from file, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Get media source interface
    if (m_pMediaSource) {
        m_pMediaSource->Release();
        m_pMediaSource = nullptr;
    }
    
    hr = pSource->QueryInterface(IID_PPV_ARGS(&m_pMediaSource));
    pSource->Release();
    
    if (FAILED(hr)) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to get media source interface, hr=" + std::to_wstring(hr));
        return false;
    }
    
    // Create the video session if not already created
    if (!CreateVideoSession()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create video session");
        return false;
    }
    
    // Create topology for the media source
    if (!CreateTopology()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create topology");
        return false;
    }
    
    // Set the topology on the session
    if (m_pSession && m_pTopology) {
        hr = m_pSession->SetTopology(0, m_pTopology);
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to set topology, hr=" + std::to_wstring(hr));
            return false;
        }
        
        // Start the media session
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_EMPTY;
        
        hr = m_pSession->Start(&GUID_NULL, &var);
        PropVariantClear(&var);
        
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to start media session, hr=" + std::to_wstring(hr));
            return false;
        }
        
        AddDebugLog(L"[SIMPLE_PLAYER] Media session started successfully from file");
        return true;
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] No media session available for playback");
    return false;
}

bool SimpleBuiltinPlayer::StartVideoPlayback() {
    AddDebugLog(L"[SIMPLE_PLAYER] Starting video playback with Media Foundation");
    
    // Create the video session if not already created
    if (!CreateVideoSession()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create video session");
        return false;
    }
    
    if (!m_pMediaSource) {
        AddDebugLog(L"[SIMPLE_PLAYER] No media source available for playback");
        return false;
    }
    
    // Create topology for the media source
    if (!CreateTopology()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to create topology");
        return false;
    }
    
    // Set the topology on the session
    if (m_pSession && m_pTopology) {
        HRESULT hr = m_pSession->SetTopology(0, m_pTopology);
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to set topology, hr=" + std::to_wstring(hr));
            return false;
        }
        
        // Start the media session
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_EMPTY;
        
        hr = m_pSession->Start(&GUID_NULL, &var);
        PropVariantClear(&var);
        
        if (FAILED(hr)) {
            AddDebugLog(L"[SIMPLE_PLAYER] Failed to start media session, hr=" + std::to_wstring(hr));
            return false;
        }
        
        AddDebugLog(L"[SIMPLE_PLAYER] Media session started successfully");
        return true;
    }
    
    AddDebugLog(L"[SIMPLE_PLAYER] No media session available for playback");
    return false;
}

bool SimpleBuiltinPlayer::ReadFromMemoryMap(const std::wstring& stream_name, std::atomic<bool>& cancel_token) {
    AddDebugLog(L"[BUILTIN_MEMORY] Starting memory map reader for stream: " + stream_name);
    
    // Create memory map reader
    StreamMemoryMap memory_map;
    
    // Try to open the memory map (with retries for startup timing)
    bool connected = false;
    for (int attempts = 0; attempts < 60; ++attempts) {  // Increased retries for memory map creation
        if (memory_map.OpenAsReader(stream_name)) {
            connected = true;
            AddDebugLog(L"[BUILTIN_MEMORY] Successfully connected to memory map: " + stream_name);
            break;
        }
        
        if (cancel_token.load()) {
            AddDebugLog(L"[BUILTIN_MEMORY] Cancelled during memory map connection");
            return false;
        }
        
        AddDebugLog(L"[BUILTIN_MEMORY] Attempt " + std::to_wstring(attempts + 1) + L" to connect to memory map failed, retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    if (!connected) {
        AddDebugLog(L"[BUILTIN_MEMORY] Failed to connect to memory map after retries");
        return false;
    }

    // Buffer for reading data
    const size_t BUFFER_SIZE = 64 * 1024; // 64KB buffer
    std::vector<char> buffer(BUFFER_SIZE);
    
    size_t total_bytes_read = 0;
    int consecutive_empty_reads = 0;
    const int MAX_EMPTY_READS = 200; // 10 seconds at 50ms intervals
    
    AddDebugLog(L"[BUILTIN_MEMORY] Starting data reading from memory map for stream: " + stream_name);
    
    while (!cancel_token.load() && m_isPlaying.load()) {
        // Read data from memory map
        size_t bytes_read = memory_map.ReadData(buffer.data(), BUFFER_SIZE);
        
        if (bytes_read > 0) {
            // Feed data to player
            if (!FeedData(buffer.data(), bytes_read)) {
                AddDebugLog(L"[BUILTIN_MEMORY] Failed to feed data to player");
                break;
            }
            
            total_bytes_read += bytes_read;
            consecutive_empty_reads = 0;
            
            // Log periodically
            if (total_bytes_read % (1024 * 1024) == 0) { // Every MB
                AddDebugLog(L"[BUILTIN_MEMORY] Read " + std::to_wstring(total_bytes_read / 1024) + L" KB from memory map");
            }
        } else {
            consecutive_empty_reads++;
            
            // Check if stream has ended
            if (memory_map.IsStreamEnded()) {
                AddDebugLog(L"[BUILTIN_MEMORY] Stream ended normally, total bytes read: " + std::to_wstring(total_bytes_read));
                break;
            }
            
            // Check if writer is still active
            if (!memory_map.IsWriterActive()) {
                AddDebugLog(L"[BUILTIN_MEMORY] Writer no longer active, ending stream");
                break;
            }
            
            // Too many empty reads - might indicate a problem
            if (consecutive_empty_reads >= MAX_EMPTY_READS) {
                AddDebugLog(L"[BUILTIN_MEMORY] Too many consecutive empty reads (" + 
                           std::to_wstring(consecutive_empty_reads) + L"), ending stream");
                break;
            }
            
            // Brief pause before retry
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    AddDebugLog(L"[BUILTIN_MEMORY] Memory map reading completed for stream: " + stream_name + 
               L", total bytes read: " + std::to_wstring(total_bytes_read));
    
    return total_bytes_read > 0;
}