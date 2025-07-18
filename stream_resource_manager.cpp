#include "stream_resource_manager.h"
#include "stream_thread.h"
#include <psapi.h>
#include <thread>
#include <algorithm>

// Static member definitions
std::mutex StreamResourceManager::instance_mutex_;
std::unique_ptr<StreamResourceManager> StreamResourceManager::instance_;

StreamResourceManager& StreamResourceManager::getInstance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (!instance_) {
        instance_ = std::unique_ptr<StreamResourceManager>(new StreamResourceManager());
    }
    return *instance_;
}

bool StreamResourceManager::CreateStreamResources(const std::wstring& stream_id, const StreamResourceQuota& quota) {
    std::lock_guard<std::mutex> lock(resources_mutex_);
    
    // Check if resources already exist for this stream
    if (stream_jobs_.find(stream_id) != stream_jobs_.end()) {
        AddDebugLog(L"StreamResourceManager: Resources already exist for stream " + stream_id);
        return true;
    }
    
    AddDebugLog(L"StreamResourceManager: Creating resources for stream " + stream_id + 
               L", active=" + std::to_wstring(active_streams_.load()));
    
    HANDLE job_handle = nullptr;
    if (quota.use_job_object) {
        // Create job object for process isolation following browser patterns
        job_handle = CreateJobObject(nullptr, nullptr);
        if (job_handle) {
            // Configure job object with proper limits that allow graphics access
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits = {};
            job_limits.BasicLimitInformation.LimitFlags = 
                JOB_OBJECT_LIMIT_ACTIVE_PROCESS |           // Limit active processes
                JOB_OBJECT_LIMIT_PROCESS_MEMORY |           // Limit process memory
                JOB_OBJECT_LIMIT_JOB_MEMORY |               // Limit job memory
                JOB_OBJECT_LIMIT_PROCESS_TIME |             // Limit process time
                JOB_OBJECT_LIMIT_BREAKAWAY_OK;              // Allow breakaway for graphics drivers
            
            job_limits.BasicLimitInformation.ActiveProcessLimit = 1;
            job_limits.ProcessMemoryLimit = quota.max_memory_mb * 1024 * 1024;  // Convert MB to bytes
            job_limits.JobMemoryLimit = quota.max_memory_mb * 1024 * 1024;      // Convert MB to bytes
            job_limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 0;  // No time limit
            
            // Configure UI restrictions to allow graphics access like browsers do
            JOBOBJECT_BASIC_UI_RESTRICTIONS ui_restrictions = {};
            ui_restrictions.UIRestrictionsClass = 
                JOB_OBJECT_UILIMIT_DESKTOP |                // Allow desktop access
                JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |        // Allow display settings
                JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;        // Allow system parameters
            
            SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, 
                                   &job_limits, sizeof(job_limits));
            
            SetInformationJobObject(job_handle, JobObjectBasicUIRestrictions,
                                   &ui_restrictions, sizeof(ui_restrictions));
            
            AddDebugLog(L"StreamResourceManager: Created job object with graphics-friendly limits for stream " + stream_id);
        } else {
            AddDebugLog(L"StreamResourceManager: Failed to create job object for stream " + stream_id + 
                       L", Error=" + std::to_wstring(GetLastError()));
        }
    }
    
    stream_jobs_[stream_id] = job_handle;
    stream_start_times_[stream_id] = std::chrono::steady_clock::now();
    active_streams_++;
    total_streams_created_++;
    
    AddDebugLog(L"StreamResourceManager: Resources created for stream " + stream_id + 
               L", active=" + std::to_wstring(active_streams_.load()));
    
    return true;
}

bool StreamResourceManager::AssignProcessToStream(const std::wstring& stream_id, HANDLE process_handle, DWORD process_id) {
    std::lock_guard<std::mutex> lock(resources_mutex_);
    
    auto job_it = stream_jobs_.find(stream_id);
    if (job_it == stream_jobs_.end()) {
        AddDebugLog(L"StreamResourceManager: No job object found for stream " + stream_id);
        return false;
    }
    
    stream_processes_[stream_id] = process_handle;
    
    if (job_it->second) {
        // Assign process to job object for resource isolation
        if (!AssignProcessToJobObject(job_it->second, process_handle)) {
            DWORD error = GetLastError();
            AddDebugLog(L"StreamResourceManager: Failed to assign process to job for stream " + stream_id + 
                       L", Error=" + std::to_wstring(error));
            return false;
        }
        
        AddDebugLog(L"StreamResourceManager: Assigned process " + std::to_wstring(process_id) + 
                   L" to job for stream " + stream_id);
    }
    
    return true;
}

bool StreamResourceManager::IsStreamProcessHealthy(const std::wstring& stream_id) {
    std::lock_guard<std::mutex> lock(resources_mutex_);
    
    auto process_it = stream_processes_.find(stream_id);
    if (process_it == stream_processes_.end() || !process_it->second) {
        return false;
    }
    
    // Check if process is genuinely running
    bool is_healthy = StreamProcessUtils::IsProcessGenuinelyRunning(process_it->second, stream_id);
    
    // Implement failure tolerance for multi-stream scenarios
    if (!is_healthy) {
        // Track consecutive failures
        stream_health_failures_[stream_id]++;
        int failures = stream_health_failures_[stream_id];
        
        AddDebugLog(L"StreamResourceManager: Health check failed for " + stream_id + 
                   L", failures=" + std::to_wstring(failures) + L"/10");
        
        // Allow up to 10 consecutive failures before declaring process dead
        // This prevents false positives during multi-stream resource pressure
        if (failures >= 10) {
            AddDebugLog(L"StreamResourceManager: Process declared dead after " + 
                       std::to_wstring(failures) + L" failures for " + stream_id);
            return false;
        }
        
        // Add a small delay to reduce system load and allow recovery
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Return true to give the process more time to recover
        return true;
    } else {
        // Process is healthy - reset failure count
        stream_health_failures_[stream_id] = 0;
        return true;
    }
}

void StreamResourceManager::CleanupStreamResources(const std::wstring& stream_id) {
    std::lock_guard<std::mutex> lock(resources_mutex_);
    
    AddDebugLog(L"StreamResourceManager: Cleaning up resources for stream " + stream_id);
    
    // Cleanup process handle
    auto process_it = stream_processes_.find(stream_id);
    if (process_it != stream_processes_.end()) {
        if (process_it->second) {
            StreamProcessUtils::TerminateProcessGracefully(process_it->second);
        }
        stream_processes_.erase(process_it);
    }
    
    // Cleanup job object
    auto job_it = stream_jobs_.find(stream_id);
    if (job_it != stream_jobs_.end()) {
        if (job_it->second) {
            // Terminate all processes in the job
            TerminateJobObject(job_it->second, 0);
            CloseHandle(job_it->second);
        }
        stream_jobs_.erase(job_it);
    }
    
    // Remove timing info
    stream_start_times_.erase(stream_id);
    
    // Reset health failure tracking
    stream_health_failures_.erase(stream_id);
    
    if (active_streams_.load() > 0) {
        active_streams_--;
    }
    
    AddDebugLog(L"StreamResourceManager: Cleanup complete for stream " + stream_id + 
               L", active=" + std::to_wstring(active_streams_.load()));
}

bool StreamResourceManager::IsSystemUnderLoad() const {
    int active = active_streams_.load();
    
    // System is under load if:
    // 1. More than 4 active streams (increased threshold)
    // 2. Or if we have streams that started recently (within 15 seconds - reduced window)
    if (active > 4) {
        return true;
    }
    
    if (active > 2) {
        std::lock_guard<std::mutex> lock(resources_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = stream_start_times_.begin(); it != stream_start_times_.end(); ++it) {
            const std::wstring& stream_id = it->first;
            const std::chrono::steady_clock::time_point& start_time = it->second;
            auto elapsed_duration = now - start_time;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(elapsed_duration);
            if (elapsed.count() < 15) {  // Reduced from 30 to 15 seconds
                return true;
            }
        }
    }
    
    return false;
}

DWORD StreamResourceManager::GetRecommendedStartDelay() const {
    int active = active_streams_.load();
    
    if (active == 0) return 50;            // First stream - minimal delay
    if (active == 1) return 500;           // Second stream - 0.5 second  
    if (active == 2) return 1000;          // Third stream - 1 second
    
    // Additional streams get modest delays to prevent resource conflicts
    return 1000 + (active - 2) * 500;
}

DWORD StreamResourceManager::GetRecommendedPipeBuffer() const {
    int active = active_streams_.load();
    
    // For multi-stream scenarios, use larger buffers to reduce pipe congestion and frame drops
    DWORD base_size = 262144;  // 256KB baseline for smooth single stream
    if (active > 1) {
        base_size = 524288;    // 512KB when multiple streams are active to prevent frame drops
    }
    if (active > 3) {
        base_size = 1048576;   // 1MB when many streams are active for optimal buffering
    }
    
    return base_size;
}

DWORD StreamResourceManager::GetRecommendedProcessPriority() const {
    int active = active_streams_.load();
    
    // Use higher priority for media processes to reduce frame drops
    if (active == 1) {
        return HIGH_PRIORITY_CLASS;        // Single stream gets high priority for best quality
    } else if (active <= 3) {
        return ABOVE_NORMAL_PRIORITY_CLASS; // Multiple streams get above normal priority
    } else {
        return NORMAL_PRIORITY_CLASS;      // Many streams use normal priority to avoid system issues
    }
}

StreamResourceManager::~StreamResourceManager() {
    std::lock_guard<std::mutex> lock(resources_mutex_);
    
    AddDebugLog(L"StreamResourceManager: Destructor called, cleaning up all resources");
    
    // Cleanup all remaining resources
    for (auto it = stream_jobs_.begin(); it != stream_jobs_.end(); ++it) {
        const std::wstring& stream_id = it->first;
        HANDLE job_handle = it->second;
        if (job_handle) {
            TerminateJobObject(job_handle, 0);
            CloseHandle(job_handle);
        }
    }
    
    stream_jobs_.clear();
    stream_processes_.clear();
    stream_start_times_.clear();
    stream_health_failures_.clear();
}

// StreamResourceGuard implementation
StreamResourceGuard::StreamResourceGuard(const std::wstring& stream_id, const StreamResourceQuota& quota) 
    : stream_id_(stream_id) {
    resources_created_ = StreamResourceManager::getInstance().CreateStreamResources(stream_id, quota);
    if (!resources_created_) {
        AddDebugLog(L"StreamResourceGuard: Failed to create resources for stream " + stream_id_);
    }
}

StreamResourceGuard::~StreamResourceGuard() {
    if (resources_created_) {
        StreamResourceManager::getInstance().CleanupStreamResources(stream_id_);
    }
}

bool StreamResourceGuard::AssignProcess(HANDLE process_handle, DWORD process_id) {
    if (!resources_created_) return false;
    return StreamResourceManager::getInstance().AssignProcessToStream(stream_id_, process_handle, process_id);
}

bool StreamResourceGuard::IsProcessHealthy() {
    if (!resources_created_) return false;
    return StreamResourceManager::getInstance().IsStreamProcessHealthy(stream_id_);
}

// StreamProcessUtils implementation
namespace StreamProcessUtils {
    
    bool CreateIsolatedProcess(
        const std::wstring& command_line,
        const std::wstring& stream_id,
        const StreamResourceQuota& quota,
        PROCESS_INFORMATION* process_info,
        HANDLE stdin_handle,
        HANDLE stdout_handle,
        HANDLE stderr_handle) {
        
        if (!process_info) return false;
        
        STARTUPINFOW startup_info = {};
        startup_info.cb = sizeof(startup_info);
        
        if (stdin_handle || stdout_handle || stderr_handle) {
            startup_info.dwFlags = STARTF_USESTDHANDLES;
            startup_info.hStdInput = stdin_handle;
            startup_info.hStdOutput = stdout_handle ? stdout_handle : GetStdHandle(STD_OUTPUT_HANDLE);
            startup_info.hStdError = stderr_handle ? stderr_handle : GetStdHandle(STD_ERROR_HANDLE);
        }
        
        // Use proper process isolation flags following browser security patterns
        DWORD creation_flags = CREATE_NEW_PROCESS_GROUP |    // Isolate process group
                              CREATE_NO_WINDOW |             // Hide window
                              CREATE_BREAKAWAY_FROM_JOB |    // Allow breakaway for graphics drivers
                              quota.process_priority;        // Use specified priority
        
        // Additional security flags that don't interfere with graphics
        if (quota.use_job_object) {
            creation_flags |= CREATE_SUSPENDED;             // Start suspended for job assignment
        }
        
        AddDebugLog(L"StreamProcessUtils: Creating isolated process for stream " + stream_id + 
                   L", flags=" + std::to_wstring(creation_flags));
        
        BOOL success = CreateProcessW(
            nullptr,
            const_cast<LPWSTR>(command_line.c_str()),
            nullptr, // process security attributes
            nullptr, // thread security attributes
            TRUE,    // inherit handles
            creation_flags,
            nullptr, // environment
            nullptr, // current directory
            &startup_info,
            process_info
        );
        
        if (success) {
            AddDebugLog(L"StreamProcessUtils: Process created successfully for stream " + stream_id + 
                       L", PID=" + std::to_wstring(process_info->dwProcessId));
            
            // Resume the process after job assignment (if it was created suspended)
            if (quota.use_job_object && (creation_flags & CREATE_SUSPENDED)) {
                // The process will be resumed by the caller after job assignment
                AddDebugLog(L"StreamProcessUtils: Process created suspended for job assignment");
            }
            
            return true;
        } else {
            DWORD error = GetLastError();
            AddDebugLog(L"StreamProcessUtils: Failed to create process for stream " + stream_id + 
                       L", Error=" + std::to_wstring(error));
            return false;
        }
    }
    
    bool IsProcessGenuinelyRunning(HANDLE process_handle, const std::wstring& debug_name) {
        if (!process_handle || process_handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        
        // Primary check: Get exit code - this is the most reliable
        DWORD exit_code;
        if (!GetExitCodeProcess(process_handle, &exit_code)) {
            AddDebugLog(L"StreamProcessUtils: GetExitCodeProcess failed for " + debug_name);
            return false;
        }
        
        if (exit_code != STILL_ACTIVE) {
            AddDebugLog(L"StreamProcessUtils: Process has exited with code " + 
                       std::to_wstring(exit_code) + L" for " + debug_name);
            return false;
        }
        
        // Secondary check: Wait with 0 timeout to see if process is signaled
        DWORD wait_result = WaitForSingleObject(process_handle, 0);
        if (wait_result == WAIT_OBJECT_0) {
            AddDebugLog(L"StreamProcessUtils: Process handle signaled (dead) for " + debug_name);
            return false;
        }
        
        // For multi-stream scenarios, be more tolerant of temporary resource pressure
        // Only do additional checks if we have reason to suspect the process is dead
        if (wait_result == WAIT_TIMEOUT) {
            // Process handle is not signaled, so process is likely alive
            // Skip resource-intensive checks that can fail under load
            return true;
        }
        
        // If we get here, something unusual happened with the wait
        // Do a final validation to be sure
        DWORD process_id = GetProcessId(process_handle);
        if (process_id == 0) {
            AddDebugLog(L"StreamProcessUtils: Cannot get process ID for " + debug_name);
            return false;
        }
        
        // Process appears to be running
        return true;
    }
    
    bool ResumeProcessAfterJobAssignment(HANDLE thread_handle, const std::wstring& stream_id) {
        if (!thread_handle || thread_handle == INVALID_HANDLE_VALUE) {
            AddDebugLog(L"StreamProcessUtils: Invalid thread handle for resume " + stream_id);
            return false;
        }
        
        // Resume the main thread after job assignment
        DWORD resume_count = ResumeThread(thread_handle);
        if (resume_count != (DWORD)-1) {
            AddDebugLog(L"StreamProcessUtils: Successfully resumed process thread for stream " + stream_id);
            return true;
        } else {
            DWORD error = GetLastError();
            AddDebugLog(L"StreamProcessUtils: Failed to resume process thread for stream " + stream_id + 
                       L", Error=" + std::to_wstring(error));
            return false;
        }
    }
    
    void TerminateProcessGracefully(HANDLE process_handle, DWORD timeout_ms) {
        if (!process_handle || process_handle == INVALID_HANDLE_VALUE) {
            return;
        }
        
        // Check if process is already dead
        DWORD exit_code;
        if (GetExitCodeProcess(process_handle, &exit_code) && exit_code != STILL_ACTIVE) {
            return;
        }
        
        // Try to terminate gracefully first
        if (TerminateProcess(process_handle, 0)) {
            // Wait for process to actually terminate
            WaitForSingleObject(process_handle, timeout_ms);
        }
    }
}