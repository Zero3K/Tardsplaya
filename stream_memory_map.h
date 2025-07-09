#pragma once
#include <windows.h>
#include <string>
#include <atomic>

// Memory-mapped file based streaming for reliable multi-stream communication
class StreamMemoryMap {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 16 * 1024 * 1024; // 16MB buffer
    static constexpr size_t HEADER_SIZE = 4096; // 4KB for control data
    
    struct ControlHeader {
        volatile LONG writer_position;      // Current write position
        volatile LONG reader_position;      // Current read position  
        volatile LONG buffer_size;          // Total buffer size
        volatile LONG data_available;       // Amount of data available
        volatile LONG stream_ended;         // Stream end flag
        volatile LONG writer_active;        // Writer process active flag
        volatile LONG reader_active;        // Reader process active flag
        volatile LONG sequence_number;      // For debugging/validation
        char padding[4064];                 // Padding to fill 4KB
    };
    
    StreamMemoryMap();
    ~StreamMemoryMap();
    
    // Writer interface (Tardsplaya side)
    bool CreateAsWriter(const std::wstring& stream_name, size_t buffer_size = DEFAULT_BUFFER_SIZE);
    bool WriteData(const void* data, size_t size, std::atomic<bool>& cancel_token);
    void SignalStreamEnd();
    bool IsReaderActive() const;
    
    // Reader interface (Media player side) 
    bool OpenAsReader(const std::wstring& stream_name);
    size_t ReadData(void* buffer, size_t max_size);
    bool IsStreamEnded() const;
    bool IsWriterActive() const;
    
    // Common interface
    void Close();
    bool IsValid() const { return mapping_handle_ != nullptr; }
    const std::wstring& GetStreamName() const { return stream_name_; }
    
    // Static helper for generating memory map names
    static std::wstring GenerateMemoryMapName(const std::wstring& stream_name);
    
private:
    std::wstring stream_name_;
    std::wstring memory_map_name_;
    HANDLE mapping_handle_;
    HANDLE mutex_handle_;
    void* mapped_memory_;
    ControlHeader* header_;
    char* data_buffer_;
    size_t buffer_size_;
    bool is_writer_;
    
    bool CreateMapping(size_t total_size);
    bool OpenMapping();
    bool AcquireLock(DWORD timeout_ms = 5000);
    void ReleaseLock();
    size_t GetAvailableWriteSpace() const;
    size_t GetAvailableReadData() const;
    bool WaitForSpace(size_t required_space, std::atomic<bool>& cancel_token);
};

// Utility functions for memory-mapped file streaming
namespace StreamMemoryMapUtils {
    // Launch media player with memory-mapped file input instead of pipe
    bool LaunchPlayerWithMemoryMap(
        const std::wstring& player_path,
        const std::wstring& stream_name,
        PROCESS_INFORMATION* pi,
        const std::wstring& channel_name = L""
    );
    
    // Helper for converting memory map to stdin for legacy players
    std::wstring CreateMemoryMapReaderHelper();
}