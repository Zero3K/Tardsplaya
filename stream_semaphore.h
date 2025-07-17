#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <memory>

// Forward declarations for debug logging
void AddDebugLog(const std::wstring& msg);

// Counting Semaphore wrapper for Windows
// Provides better IPC flow control than basic mutexes
class StreamSemaphore {
public:
    // Constructor: creates or opens a named semaphore
    // initial_count: starting value (typically 0 for producer/consumer)
    // max_count: maximum value the semaphore can reach
    // name: optional name for cross-process semaphores
    StreamSemaphore(LONG initial_count = 0, LONG max_count = 1000, const std::wstring& name = L"");
    ~StreamSemaphore();

    // Wait for semaphore (decrement count, blocks if 0)
    bool Wait(DWORD timeout_ms = INFINITE);
    
    // Signal semaphore (increment count)
    bool Signal(LONG count = 1);
    
    // Try to wait without blocking
    bool TryWait();
    
    // Get current count (approximate, for debugging)
    LONG GetCount() const;
    
    // Check if semaphore is valid
    bool IsValid() const { return semaphore_handle_ != nullptr; }
    
    // Get the semaphore name (for debugging)
    const std::wstring& GetName() const { return name_; }

private:
    HANDLE semaphore_handle_;
    std::wstring name_;
    LONG max_count_;
    mutable std::atomic<LONG> approximate_count_;
    
    // Disable copy constructor and assignment
    StreamSemaphore(const StreamSemaphore&) = delete;
    StreamSemaphore& operator=(const StreamSemaphore&) = delete;
};

// Helper class for producer-consumer pattern with semaphores
class ProducerConsumerSemaphores {
public:
    ProducerConsumerSemaphores(LONG buffer_size, const std::wstring& base_name = L"");
    ~ProducerConsumerSemaphores() = default;
    
    // Producer interface
    bool WaitForProduceSlot(DWORD timeout_ms = INFINITE);
    bool SignalItemProduced();
    
    // Consumer interface  
    bool WaitForConsumeItem(DWORD timeout_ms = INFINITE);
    bool SignalItemConsumed();
    
    // Status
    bool IsValid() const;
    LONG GetApproximateItemCount() const;
    LONG GetApproximateFreeSlots() const;
    
private:
    StreamSemaphore empty_slots_;  // Tracks available buffer slots
    StreamSemaphore filled_slots_; // Tracks items ready for consumption
    LONG buffer_size_;
};

// Utility functions for semaphore-based IPC
namespace StreamSemaphoreUtils {
    // Generate unique semaphore name for a stream
    std::wstring GenerateSemaphoreName(const std::wstring& stream_id, const std::wstring& type);
    
    // Create producer-consumer semaphores for a specific stream
    std::unique_ptr<ProducerConsumerSemaphores> CreateStreamSemaphores(
        const std::wstring& stream_id, 
        LONG buffer_size = 50 // Default buffer size for stream segments
    );
}