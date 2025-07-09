#define NOMINMAX
#include "stream_memory_map.h"
#include "stream_thread.h"
#include <sstream>
#include <chrono>
#include <thread>
#include <algorithm>

StreamMemoryMap::StreamMemoryMap() 
    : mapping_handle_(nullptr)
    , mutex_handle_(nullptr)
    , mapped_memory_(nullptr)
    , header_(nullptr)
    , data_buffer_(nullptr)
    , buffer_size_(0)
    , is_writer_(false) {
}

StreamMemoryMap::~StreamMemoryMap() {
    Close();
}

std::wstring StreamMemoryMap::GenerateMemoryMapName(const std::wstring& stream_name) {
    // Generate unique memory map name based on stream name and timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::wstringstream ss;
    ss << L"TardsplayaStream_" << stream_name << L"_" << time_t << L"_" << ms.count();
    
    // Replace invalid characters for memory map names
    std::wstring result = ss.str();
    for (auto& c : result) {
        if (!isalnum(c) && c != L'_') {
            c = L'_';
        }
    }
    
    return result;
}

bool StreamMemoryMap::CreateAsWriter(const std::wstring& stream_name, size_t buffer_size) {
    if (IsValid()) {
        AddDebugLog(L"StreamMemoryMap::CreateAsWriter: Already initialized");
        return false;
    }
    
    stream_name_ = stream_name;
    // Use simplified naming scheme for consistency
    memory_map_name_ = L"TardsplayaStream_" + stream_name;
    buffer_size_ = buffer_size;
    is_writer_ = true;
    
    size_t total_size = HEADER_SIZE + buffer_size;
    
    AddDebugLog(L"StreamMemoryMap::CreateAsWriter: Creating memory map " + memory_map_name_ + 
               L", buffer_size=" + std::to_wstring(buffer_size) + 
               L", total_size=" + std::to_wstring(total_size));
    
    if (!CreateMapping(total_size)) {
        AddDebugLog(L"StreamMemoryMap::CreateAsWriter: Failed to create mapping");
        return false;
    }
    
    // Initialize control header
    ZeroMemory(header_, sizeof(ControlHeader));
    header_->buffer_size = static_cast<LONG>(buffer_size);
    header_->writer_active = 1;
    header_->sequence_number = 1;
    
    AddDebugLog(L"StreamMemoryMap::CreateAsWriter: Successfully created for " + stream_name);
    return true;
}

bool StreamMemoryMap::OpenAsReader(const std::wstring& stream_name) {
    if (IsValid()) {
        AddDebugLog(L"StreamMemoryMap::OpenAsReader: Already initialized");
        return false;
    }
    
    stream_name_ = stream_name;
    // Use same naming scheme as writer
    memory_map_name_ = L"TardsplayaStream_" + stream_name;
    is_writer_ = false;
    
    AddDebugLog(L"StreamMemoryMap::OpenAsReader: Opening memory map " + memory_map_name_);
    
    if (!OpenMapping()) {
        AddDebugLog(L"StreamMemoryMap::OpenAsReader: Failed to open mapping");
        return false;
    }
    
    // Signal reader is active
    if (AcquireLock()) {
        header_->reader_active = 1;
        ReleaseLock();
    }
    
    AddDebugLog(L"StreamMemoryMap::OpenAsReader: Successfully opened for " + stream_name);
    return true;
}

bool StreamMemoryMap::CreateMapping(size_t total_size) {
    // Create mutex for synchronization
    std::wstring mutex_name = memory_map_name_ + L"_Mutex";
    mutex_handle_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (!mutex_handle_) {
        AddDebugLog(L"StreamMemoryMap::CreateMapping: Failed to create mutex, Error=" + 
                   std::to_wstring(GetLastError()));
        return false;
    }
    
    // Create file mapping
    mapping_handle_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,   // Use paging file
        nullptr,                // Default security
        PAGE_READWRITE,         // Read/write access
        0,                      // High-order DWORD of size
        static_cast<DWORD>(total_size), // Low-order DWORD of size
        memory_map_name_.c_str() // Name
    );
    
    if (!mapping_handle_) {
        AddDebugLog(L"StreamMemoryMap::CreateMapping: Failed to create file mapping, Error=" + 
                   std::to_wstring(GetLastError()));
        CloseHandle(mutex_handle_);
        mutex_handle_ = nullptr;
        return false;
    }
    
    // Map view of file
    mapped_memory_ = MapViewOfFile(
        mapping_handle_,
        FILE_MAP_ALL_ACCESS,
        0, 0,  // Offset
        total_size
    );
    
    if (!mapped_memory_) {
        AddDebugLog(L"StreamMemoryMap::CreateMapping: Failed to map view of file, Error=" + 
                   std::to_wstring(GetLastError()));
        CloseHandle(mapping_handle_);
        CloseHandle(mutex_handle_);
        mapping_handle_ = nullptr;
        mutex_handle_ = nullptr;
        return false;
    }
    
    header_ = static_cast<ControlHeader*>(mapped_memory_);
    data_buffer_ = static_cast<char*>(mapped_memory_) + HEADER_SIZE;
    
    return true;
}

bool StreamMemoryMap::OpenMapping() {
    // Open existing mutex
    std::wstring mutex_name = memory_map_name_ + L"_Mutex";
    mutex_handle_ = OpenMutexW(SYNCHRONIZE, FALSE, mutex_name.c_str());
    if (!mutex_handle_) {
        AddDebugLog(L"StreamMemoryMap::OpenMapping: Failed to open mutex, Error=" + 
                   std::to_wstring(GetLastError()));
        return false;
    }
    
    // Open existing file mapping
    mapping_handle_ = OpenFileMappingW(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        memory_map_name_.c_str()
    );
    
    if (!mapping_handle_) {
        AddDebugLog(L"StreamMemoryMap::OpenMapping: Failed to open file mapping, Error=" + 
                   std::to_wstring(GetLastError()));
        CloseHandle(mutex_handle_);
        mutex_handle_ = nullptr;
        return false;
    }
    
    // Map view of file
    mapped_memory_ = MapViewOfFile(
        mapping_handle_,
        FILE_MAP_ALL_ACCESS,
        0, 0, 0  // Map entire file
    );
    
    if (!mapped_memory_) {
        AddDebugLog(L"StreamMemoryMap::OpenMapping: Failed to map view of file, Error=" + 
                   std::to_wstring(GetLastError()));
        CloseHandle(mapping_handle_);
        CloseHandle(mutex_handle_);
        mapping_handle_ = nullptr;
        mutex_handle_ = nullptr;
        return false;
    }
    
    header_ = static_cast<ControlHeader*>(mapped_memory_);
    data_buffer_ = static_cast<char*>(mapped_memory_) + HEADER_SIZE;
    buffer_size_ = header_->buffer_size;
    
    return true;
}

bool StreamMemoryMap::AcquireLock(DWORD timeout_ms) {
    if (!mutex_handle_) return false;
    
    DWORD result = WaitForSingleObject(mutex_handle_, timeout_ms);
    return (result == WAIT_OBJECT_0);
}

void StreamMemoryMap::ReleaseLock() {
    if (mutex_handle_) {
        ReleaseMutex(mutex_handle_);
    }
}

size_t StreamMemoryMap::GetAvailableWriteSpace() const {
    if (!header_) return 0;
    
    LONG writer_pos = header_->writer_position;
    LONG reader_pos = header_->reader_position;
    LONG buf_size = header_->buffer_size;
    
    if (writer_pos >= reader_pos) {
        // Normal case: writer ahead of reader
        size_t used_space = writer_pos - reader_pos;
        return buf_size - used_space - 1; // Leave 1 byte to distinguish full from empty
    } else {
        // Wrap-around case: reader ahead of writer
        return reader_pos - writer_pos - 1;
    }
}

size_t StreamMemoryMap::GetAvailableReadData() const {
    if (!header_) return 0;
    
    LONG writer_pos = header_->writer_position;
    LONG reader_pos = header_->reader_position;
    LONG buf_size = header_->buffer_size;
    
    if (writer_pos >= reader_pos) {
        return writer_pos - reader_pos;
    } else {
        // Wrap-around case
        return (buf_size - reader_pos) + writer_pos;
    }
}

bool StreamMemoryMap::WaitForSpace(size_t required_space, std::atomic<bool>& cancel_token) {
    const int max_wait_ms = 10000; // 10 seconds max
    const int sleep_ms = 50;
    int total_wait = 0;
    
    while (total_wait < max_wait_ms && !cancel_token.load()) {
        if (AcquireLock(1000)) {
            size_t available = GetAvailableWriteSpace();
            bool has_space = available >= required_space;
            bool reader_alive = header_->reader_active != 0;
            ReleaseLock();
            
            if (has_space) {
                return true;
            }
            
            if (!reader_alive) {
                AddDebugLog(L"StreamMemoryMap::WaitForSpace: Reader no longer active");
                return false;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        total_wait += sleep_ms;
    }
    
    AddDebugLog(L"StreamMemoryMap::WaitForSpace: Timeout waiting for space, required=" + 
               std::to_wstring(required_space));
    return false;
}

bool StreamMemoryMap::WriteData(const void* data, size_t size, std::atomic<bool>& cancel_token) {
    if (!IsValid() || !is_writer_ || !data || size == 0) {
        return false;
    }
    
    const char* input_data = static_cast<const char*>(data);
    size_t remaining = size;
    
    AddDebugLog(L"StreamMemoryMap::WriteData: Writing " + std::to_wstring(size) + 
               L" bytes to " + stream_name_);
    
    while (remaining > 0 && !cancel_token.load()) {
        if (!AcquireLock(5000)) {
            AddDebugLog(L"StreamMemoryMap::WriteData: Failed to acquire lock");
            return false;
        }
        
        size_t available_space = GetAvailableWriteSpace();
        if (available_space == 0) {
            ReleaseLock();
            if (!WaitForSpace(1, cancel_token)) {
                return false;
            }
            continue;
        }
        
        size_t to_write = (std::min)(remaining, available_space);
        LONG writer_pos = header_->writer_position;
        LONG buf_size = header_->buffer_size;
        
        // Handle wrap-around
        if (writer_pos + static_cast<LONG>(to_write) <= buf_size) {
            // No wrap-around needed
            memcpy(data_buffer_ + writer_pos, input_data, to_write);
        } else {
            // Wrap-around needed
            size_t first_chunk = buf_size - writer_pos;
            size_t second_chunk = to_write - first_chunk;
            
            memcpy(data_buffer_ + writer_pos, input_data, first_chunk);
            memcpy(data_buffer_, input_data + first_chunk, second_chunk);
        }
        
        // Update writer position
        header_->writer_position = (writer_pos + static_cast<LONG>(to_write)) % buf_size;
        header_->data_available += static_cast<LONG>(to_write);
        header_->sequence_number++;
        
        ReleaseLock();
        
        input_data += to_write;
        remaining -= to_write;
    }
    
    return remaining == 0;
}

size_t StreamMemoryMap::ReadData(void* buffer, size_t max_size) {
    if (!IsValid() || is_writer_ || !buffer || max_size == 0) {
        return 0;
    }
    
    if (!AcquireLock(5000)) {
        return 0;
    }
    
    size_t available_data = GetAvailableReadData();
    if (available_data == 0) {
        ReleaseLock();
        return 0;
    }
    
    size_t to_read = (std::min)(max_size, available_data);
    LONG reader_pos = header_->reader_position;
    LONG buf_size = header_->buffer_size;
    
    char* output_buffer = static_cast<char*>(buffer);
    
    // Handle wrap-around
    if (reader_pos + static_cast<LONG>(to_read) <= buf_size) {
        // No wrap-around needed
        memcpy(output_buffer, data_buffer_ + reader_pos, to_read);
    } else {
        // Wrap-around needed
        size_t first_chunk = buf_size - reader_pos;
        size_t second_chunk = to_read - first_chunk;
        
        memcpy(output_buffer, data_buffer_ + reader_pos, first_chunk);
        memcpy(output_buffer + first_chunk, data_buffer_, second_chunk);
    }
    
    // Update reader position
    header_->reader_position = (reader_pos + static_cast<LONG>(to_read)) % buf_size;
    header_->data_available -= static_cast<LONG>(to_read);
    
    ReleaseLock();
    
    return to_read;
}

void StreamMemoryMap::SignalStreamEnd() {
    if (!IsValid() || !is_writer_) return;
    
    if (AcquireLock()) {
        header_->stream_ended = 1;
        header_->writer_active = 0;
        ReleaseLock();
    }
    
    AddDebugLog(L"StreamMemoryMap::SignalStreamEnd: Stream end signaled for " + stream_name_);
}

bool StreamMemoryMap::IsStreamEnded() const {
    if (!IsValid()) return true;
    
    return header_->stream_ended != 0 && GetAvailableReadData() == 0;
}

bool StreamMemoryMap::IsReaderActive() const {
    if (!IsValid()) return false;
    return header_->reader_active != 0;
}

bool StreamMemoryMap::IsWriterActive() const {
    if (!IsValid()) return false;
    return header_->writer_active != 0;
}

void StreamMemoryMap::Close() {
    // Signal we're no longer active
    if (IsValid() && AcquireLock(1000)) {
        if (is_writer_) {
            header_->writer_active = 0;
        } else {
            header_->reader_active = 0;
        }
        ReleaseLock();
    }
    
    if (mapped_memory_) {
        UnmapViewOfFile(mapped_memory_);
        mapped_memory_ = nullptr;
    }
    
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    
    if (mutex_handle_) {
        CloseHandle(mutex_handle_);
        mutex_handle_ = nullptr;
    }
    
    header_ = nullptr;
    data_buffer_ = nullptr;
    buffer_size_ = 0;
}

namespace StreamMemoryMapUtils {

bool LaunchPlayerWithMemoryMap(
    const std::wstring& player_path,
    const std::wstring& stream_name,
    PROCESS_INFORMATION* pi,
    const std::wstring& channel_name) {
        
    if (!pi) return false;
    
    // Create a helper executable name for memory map reading
    std::wstring helper_path = CreateMemoryMapReaderHelper();
    if (helper_path.empty()) {
        AddDebugLog(L"LaunchPlayerWithMemoryMap: Failed to create helper");
        return false;
    }
    
    // Create command line: helper.exe memory_map_name | player.exe -
    std::wstring memory_map_name = StreamMemoryMap::GenerateMemoryMapName(stream_name);
    std::wstring cmd = L"\"" + helper_path + L"\" \"" + memory_map_name + L"\" | \"" + player_path + L"\" -";
    
    AddDebugLog(L"LaunchPlayerWithMemoryMap: Command: " + cmd);
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;   // Will be connected via pipe
    si.hStdOutput = nullptr;  // Inherit
    si.hStdError = nullptr;   // Inherit
    
    BOOL success = CreateProcessW(
        nullptr,                // Application name
        const_cast<LPWSTR>(cmd.c_str()), // Command line
        nullptr,                // Process security attributes
        nullptr,                // Thread security attributes
        FALSE,                  // Inherit handles
        CREATE_NEW_CONSOLE,     // Creation flags
        nullptr,                // Environment
        nullptr,                // Current directory
        &si,                    // Startup info
        pi                      // Process information
    );
    
    if (!success) {
        AddDebugLog(L"LaunchPlayerWithMemoryMap: Failed to create process, Error=" + 
                   std::to_wstring(GetLastError()));
        return false;
    }
    
    AddDebugLog(L"LaunchPlayerWithMemoryMap: Successfully launched for " + channel_name + 
               L", PID=" + std::to_wstring(pi->dwProcessId));
    
    return true;
}

std::wstring CreateMemoryMapReaderHelper() {
    // For now, return empty to indicate this approach needs more work
    // In a full implementation, we'd create a small helper executable
    // that reads from memory map and outputs to stdout
    AddDebugLog(L"CreateMemoryMapReaderHelper: Helper creation not yet implemented");
    return L"";
}

} // namespace StreamMemoryMapUtils