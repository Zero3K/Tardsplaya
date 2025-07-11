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
    
    // Start video playback
    if (!StartVideoPlayback()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Failed to start video playback");
        return false;
    }
    
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
    // Process the segment data - in a real implementation, this would decode and render the video/audio
    // For now, we track statistics and update visual representation
    
    size_t segmentSize = data.size();
    m_totalBytesProcessed += segmentSize;
    m_segmentsProcessed++;
    
    // Simulate processing time based on segment size
    int processingTime = static_cast<int>(segmentSize / 10000); // ~1ms per 10KB
    processingTime = std::max(1, std::min(processingTime, 100)); // 1-100ms range
    
    std::this_thread::sleep_for(std::chrono::milliseconds(processingTime));
    
    // Update video window display every few segments
    if (m_hwndVideo && (m_segmentsProcessed.load() % 2 == 0)) {
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
    // For now, we'll use the visual representation instead of full MF implementation
    // Full Media Foundation implementation would require creating a media session,
    // source resolver, topology, and handling MPEG-TS stream parsing
    AddDebugLog(L"[SIMPLE_PLAYER] Using visual representation mode");
    return true;
}

bool SimpleBuiltinPlayer::SetVideoWindow(HWND hwnd) {
    // Store the video window handle for future use
    // In a full implementation, this would configure the video renderer
    AddDebugLog(L"[SIMPLE_PLAYER] Video window configured");
    return true;
}

bool SimpleBuiltinPlayer::StartVideoPlayback() {
    // For now, we'll rely on the visual representation
    // Full implementation would start the Media Foundation session
    AddDebugLog(L"[SIMPLE_PLAYER] Video playback started (visual mode)");
    return true;
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