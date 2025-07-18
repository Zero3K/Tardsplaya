#include "stream_semaphore.h"
#include <sstream>
#include <chrono>
#include <memory>

// External debug logging function
void AddDebugLog(const std::wstring& msg);

StreamSemaphore::StreamSemaphore(LONG initial_count, LONG max_count, const std::wstring& name)
    : semaphore_handle_(nullptr), name_(name), max_count_(max_count), approximate_count_(initial_count) {
    
    if (name.empty()) {
        // Create unnamed semaphore for single-process use
        semaphore_handle_ = CreateSemaphoreW(
            nullptr,        // default security attributes
            initial_count,  // initial count
            max_count,      // maximum count
            nullptr         // unnamed
        );
    } else {
        // Create named semaphore for cross-process use
        semaphore_handle_ = CreateSemaphoreW(
            nullptr,        // default security attributes
            initial_count,  // initial count
            max_count,      // maximum count
            name.c_str()    // semaphore name
        );
        
        if (semaphore_handle_ && GetLastError() == ERROR_ALREADY_EXISTS) {
            // Semaphore already exists, we're opening an existing one
            AddDebugLog(L"StreamSemaphore: Opened existing semaphore: " + name);
        }
    }
    
    if (!semaphore_handle_) {
        DWORD error = GetLastError();
        AddDebugLog(L"StreamSemaphore: Failed to create semaphore '" + name + 
                   L"', Error=" + std::to_wstring(error));
    } else {
        if (!name.empty()) {
            AddDebugLog(L"StreamSemaphore: Created semaphore '" + name + 
                       L"' with initial_count=" + std::to_wstring(initial_count) + 
                       L", max_count=" + std::to_wstring(max_count));
        }
    }
}

StreamSemaphore::~StreamSemaphore() {
    if (semaphore_handle_) {
        CloseHandle(semaphore_handle_);
        semaphore_handle_ = nullptr;
        
        if (!name_.empty()) {
            AddDebugLog(L"StreamSemaphore: Closed semaphore: " + name_);
        }
    }
}

bool StreamSemaphore::Wait(DWORD timeout_ms) {
    if (!semaphore_handle_) return false;
    
    DWORD result = WaitForSingleObject(semaphore_handle_, timeout_ms);
    
    if (result == WAIT_OBJECT_0) {
        // Successfully acquired semaphore, decrement our approximate count
        approximate_count_.fetch_sub(1);
        return true;
    } else if (result == WAIT_TIMEOUT) {
        // Timeout occurred
        return false;
    } else {
        // Error occurred
        DWORD error = GetLastError();
        AddDebugLog(L"StreamSemaphore: Wait failed for '" + name_ + 
                   L"', Error=" + std::to_wstring(error));
        return false;
    }
}

bool StreamSemaphore::Signal(LONG count) {
    if (!semaphore_handle_) return false;
    
    LONG previous_count = 0;
    BOOL result = ReleaseSemaphore(semaphore_handle_, count, &previous_count);
    
    if (result) {
        // Successfully released semaphore, increment our approximate count
        approximate_count_.fetch_add(count);
        return true;
    } else {
        DWORD error = GetLastError();
        AddDebugLog(L"StreamSemaphore: Signal failed for '" + name_ + 
                   L"', count=" + std::to_wstring(count) + 
                   L", Error=" + std::to_wstring(error));
        return false;
    }
}

bool StreamSemaphore::TryWait() {
    return Wait(0); // 0 timeout = don't block
}

LONG StreamSemaphore::GetCount() const {
    // Return approximate count (may not be perfectly accurate due to race conditions)
    return approximate_count_.load();
}

ProducerConsumerSemaphores::ProducerConsumerSemaphores(LONG buffer_size, const std::wstring& base_name)
    : empty_slots_(buffer_size, buffer_size, base_name.empty() ? L"" : base_name + L"_empty")
    , filled_slots_(0, buffer_size, base_name.empty() ? L"" : base_name + L"_filled")
    , buffer_size_(buffer_size) {
    
    if (!empty_slots_.IsValid() || !filled_slots_.IsValid()) {
        AddDebugLog(L"ProducerConsumerSemaphores: Failed to create semaphores for '" + base_name + L"'");
    } else {
        AddDebugLog(L"ProducerConsumerSemaphores: Created semaphores for '" + base_name + 
                   L"' with buffer_size=" + std::to_wstring(buffer_size));
    }
}

bool ProducerConsumerSemaphores::WaitForProduceSlot(DWORD timeout_ms) {
    // Wait for an empty slot to become available
    return empty_slots_.Wait(timeout_ms);
}

bool ProducerConsumerSemaphores::SignalItemProduced() {
    // Signal that an item has been produced
    return filled_slots_.Signal(1);
}

bool ProducerConsumerSemaphores::WaitForConsumeItem(DWORD timeout_ms) {
    // Wait for a filled slot to become available
    return filled_slots_.Wait(timeout_ms);
}

bool ProducerConsumerSemaphores::SignalItemConsumed() {
    // Signal that an item has been consumed (free up a slot)
    return empty_slots_.Signal(1);
}

bool ProducerConsumerSemaphores::IsValid() const {
    return empty_slots_.IsValid() && filled_slots_.IsValid();
}

LONG ProducerConsumerSemaphores::GetApproximateItemCount() const {
    return filled_slots_.GetCount();
}

LONG ProducerConsumerSemaphores::GetApproximateFreeSlots() const {
    return empty_slots_.GetCount();
}

namespace StreamSemaphoreUtils {

std::wstring GenerateSemaphoreName(const std::wstring& stream_id, const std::wstring& type) {
    // Generate a unique semaphore name based on stream ID and type
    // Include process ID to avoid conflicts between different instances
    DWORD process_id = GetCurrentProcessId();
    
    std::wostringstream oss;
    oss << L"Tardsplaya_" << process_id << L"_" << stream_id << L"_" << type;
    
    // Replace any invalid characters for Windows object names
    std::wstring name = oss.str();
    for (auto& ch : name) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || 
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            ch = L'_';
        }
    }
    
    return name;
}

std::unique_ptr<ProducerConsumerSemaphores> CreateStreamSemaphores(
    const std::wstring& stream_id, 
    LONG buffer_size) {
    
    std::wstring base_name = GenerateSemaphoreName(stream_id, L"buffer");
    
    auto semaphores = std::make_unique<ProducerConsumerSemaphores>(buffer_size, base_name);
    
    if (!semaphores->IsValid()) {
        AddDebugLog(L"StreamSemaphoreUtils: Failed to create semaphores for stream: " + stream_id);
        return nullptr;
    }
    
    AddDebugLog(L"StreamSemaphoreUtils: Created semaphores for stream '" + stream_id + 
               L"' with buffer_size=" + std::to_wstring(buffer_size));
    
    return semaphores;
}

} // namespace StreamSemaphoreUtils