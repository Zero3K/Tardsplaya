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

// SimpleBuiltinPlayer implementation
SimpleBuiltinPlayer::SimpleBuiltinPlayer()
    : m_hwndStatus(nullptr)
    , m_hwndVideo(nullptr)
    , m_isPlaying(false)
    , m_isInitialized(false)
    , m_processRunning(false)
    , m_totalBytesProcessed(0)
    , m_segmentsProcessed(0)
{
}

SimpleBuiltinPlayer::~SimpleBuiltinPlayer() {
    Cleanup();
}

bool SimpleBuiltinPlayer::Initialize(HWND hwndStatus) {
    AddDebugLog(L"[SIMPLE_PLAYER] Initializing simple built-in player");
    
    if (m_isInitialized.load()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Player already initialized");
        return true;
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
    
    // Start the processing thread
    m_processRunning = true;
    m_processThread = std::thread(&SimpleBuiltinPlayer::ProcessThreadProc, this);
    
    m_isPlaying = true;
    UpdateStatus();
    
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
    // Simulate processing the segment data
    // In a real implementation, this would decode and render the video/audio
    
    size_t segmentSize = data.size();
    m_totalBytesProcessed += segmentSize;
    m_segmentsProcessed++;
    
    // Simulate processing time based on segment size
    int processingTime = static_cast<int>(segmentSize / 10000); // ~1ms per 10KB
    processingTime = std::max(1, std::min(processingTime, 100)); // 1-100ms range
    
    std::this_thread::sleep_for(std::chrono::milliseconds(processingTime));
    
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
            
            // Draw stream info
            if (player && player->IsPlaying()) {
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkColor(hdc, RGB(0, 0, 0));
                
                std::wstring text = L"Stream: " + player->m_currentStreamName + L"\n";
                text += L"Quality: " + player->m_currentQuality + L"\n";
                text += L"Segments: " + std::to_wstring(player->GetSegmentsProcessed()) + L"\n";
                text += L"Data: " + std::to_wstring(player->GetTotalBytesProcessed() / 1024) + L" KB";
                
                DrawText(hdc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            } else {
                SetTextColor(hdc, RGB(128, 128, 128));
                SetBkColor(hdc, RGB(0, 0, 0));
                DrawText(hdc, L"Video window ready...", -1, &rect, DT_CENTER | DT_VCENTER);
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