#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_  // Prevent winsock.h conflicts  
#define NOMINMAX      // Prevent Windows min/max macros
#include <windows.h>
#include "builtin_player.h"
#include "stream_thread.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>

// SimpleBuiltinPlayer implementation
SimpleBuiltinPlayer::SimpleBuiltinPlayer()
    : m_hwndStatus(nullptr)
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

bool SimpleBuiltinPlayer::StartStream(const std::wstring& streamName) {
    AddDebugLog(L"[SIMPLE_PLAYER] Starting stream: " + streamName);
    
    if (!m_isInitialized.load()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Player not initialized");
        return false;
    }
    
    if (m_isPlaying.load()) {
        AddDebugLog(L"[SIMPLE_PLAYER] Already playing, stopping current stream");
        StopStream();
    }
    
    m_currentStreamName = streamName;
    m_totalBytesProcessed = 0;
    m_segmentsProcessed = 0;
    
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

void SimpleBuiltinPlayer::UpdateStatus() {
    if (!m_hwndStatus) return;
    
    std::wstring statusText = FormatStatus();
    SetWindowTextW(m_hwndStatus, statusText.c_str());
}

void SimpleBuiltinPlayer::Cleanup() {
    AddDebugLog(L"[SIMPLE_PLAYER] Cleaning up simple built-in player");
    
    StopStream();
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