#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>

// Stream Resource Manager - Provides proper resource isolation for multiple streams
// This addresses the resource contention issues that cause streams to interfere with each other

struct StreamResourceQuota {
    DWORD max_memory_mb = 512;          // Memory limit for video rendering
    DWORD max_handles = 100;            // Handles for graphics resources
    DWORD max_threads = 8;              // Threads for media processing
    DWORD pipe_buffer_size = 262144;    // 256KB pipe buffer per stream for reduced frame drops
    DWORD process_priority = ABOVE_NORMAL_PRIORITY_CLASS; // Higher priority for smoother playback
    bool use_job_object = true;         // Enable job objects for proper resource isolation
};

class StreamResourceManager {
private:
    static std::mutex instance_mutex_;
    static std::unique_ptr<StreamResourceManager> instance_;
    
    mutable std::mutex resources_mutex_;
    std::map<std::wstring, HANDLE> stream_jobs_;         // Job objects per stream
    std::map<std::wstring, HANDLE> stream_processes_;    // Process handles per stream
    std::map<std::wstring, std::chrono::steady_clock::time_point> stream_start_times_;
    std::map<std::wstring, int> stream_health_failures_; // Track consecutive health check failures
    std::atomic<int> active_streams_{0};
    std::atomic<int> total_streams_created_{0};
    
    StreamResourceManager() = default;
    
public:
    static StreamResourceManager& getInstance();
    
    // Stream lifecycle management
    bool CreateStreamResources(const std::wstring& stream_id, const StreamResourceQuota& quota);
    bool AssignProcessToStream(const std::wstring& stream_id, HANDLE process_handle, DWORD process_id);
    bool IsStreamProcessHealthy(const std::wstring& stream_id);
    void CleanupStreamResources(const std::wstring& stream_id);
    
    // Resource monitoring
    int GetActiveStreamCount() const { return active_streams_.load(); }
    int GetTotalStreamsCreated() const { return total_streams_created_.load(); }
    bool IsSystemUnderLoad() const;
    
    // Resource allocation helpers
    DWORD GetRecommendedStartDelay() const;
    DWORD GetRecommendedPipeBuffer() const;
    DWORD GetRecommendedProcessPriority() const;
    
    ~StreamResourceManager();
};

// Helper class for RAII resource management
class StreamResourceGuard {
private:
    std::wstring stream_id_;
    bool resources_created_ = false;
    
public:
    StreamResourceGuard(const std::wstring& stream_id, const StreamResourceQuota& quota);
    ~StreamResourceGuard();
    
    bool IsValid() const { return resources_created_; }
    bool AssignProcess(HANDLE process_handle, DWORD process_id);
    bool IsProcessHealthy();
    
    // Non-copyable
    StreamResourceGuard(const StreamResourceGuard&) = delete;
    StreamResourceGuard& operator=(const StreamResourceGuard&) = delete;
};

// Utility functions for process management
namespace StreamProcessUtils {
    // Create a process with proper resource isolation
    bool CreateIsolatedProcess(
        const std::wstring& command_line,
        const std::wstring& stream_id,
        const StreamResourceQuota& quota,
        PROCESS_INFORMATION* process_info,
        HANDLE stdin_handle = nullptr,
        HANDLE stdout_handle = nullptr,
        HANDLE stderr_handle = nullptr
    );
    
    // Resume process after job assignment (for processes created suspended)
    bool ResumeProcessAfterJobAssignment(HANDLE thread_handle, const std::wstring& stream_id);
    
    // Check if process is genuinely running (not just suspended due to resource pressure)
    bool IsProcessGenuinelyRunning(HANDLE process_handle, const std::wstring& debug_name = L"");
    
    // Terminate process gracefully with proper cleanup
    void TerminateProcessGracefully(HANDLE process_handle, DWORD timeout_ms = 5000);
}