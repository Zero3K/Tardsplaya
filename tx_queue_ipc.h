#pragma once

// TX-Queue IPC Implementation for Tardsplaya
// Provides high-performance producer/consumer communication using tx-queue
// with named pipes for media player integration

#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Include tx-queue headers
#include "tx_queue_wrapper.h"

// Forward declarations
void AddDebugLog(const std::wstring& msg);

namespace tardsplaya {

// Segment data structure for tx-queue communication
struct StreamSegment {
    std::vector<char> data;
    uint64_t sequence_number;
    uint32_t checksum;
    bool is_end_marker;
    bool has_discontinuity; // New field for discontinuity detection
    
    StreamSegment() : sequence_number(0), checksum(0), is_end_marker(false), has_discontinuity(false) {}
    explicit StreamSegment(std::vector<char>&& segment_data, uint64_t seq = 0, bool discontinuity = false)
        : data(std::move(segment_data)), sequence_number(seq), checksum(0), is_end_marker(false), has_discontinuity(discontinuity) {
        calculate_checksum();
    }
    
    void calculate_checksum() {
        checksum = 0;
        for (char byte : data) {
            checksum += static_cast<uint32_t>(byte);
        }
    }
    
    bool verify_checksum() const {
        uint32_t calculated = 0;
        for (char byte : data) {
            calculated += static_cast<uint32_t>(byte);
        }
        return calculated == checksum;
    }
    
    // Check if data contains valid MPEG-TS packets (sync byte 0x47)
    bool has_valid_ts_headers() const {
        if (data.size() < 188) return false; // Minimum TS packet size
        
        // Check for TS sync bytes at 188-byte intervals
        // Allow up to 10% bad packets to handle corrupted/encrypted content
        size_t total_packets = data.size() / 188;
        size_t valid_packets = 0;
        
        for (size_t i = 0; i < data.size() - 188; i += 188) {
            if (static_cast<unsigned char>(data[i]) == 0x47) {
                valid_packets++;
            }
        }
        
        // Require at least 90% valid sync bytes
        return (total_packets > 0) && ((valid_packets * 100 / total_packets) >= 90);
    }
};

// TX-Queue based IPC manager for high-performance streaming
class TxQueueIPC {
public:
    // Constructor - creates tx-queue with specified capacity (must be power of 2)
    explicit TxQueueIPC(uint64_t queue_capacity = 8 * 1024 * 1024); // 8MB default
    
    // Destructor
    ~TxQueueIPC();
    
    // Initialize the IPC system
    bool Initialize();
    
    // Check if IPC is ready
    bool IsReady() const { return initialized_ && queue_ && queue_->is_ok(); }
    
    // Producer interface - add stream segment to queue
    bool ProduceSegment(std::vector<char>&& segment_data, bool has_discontinuity = false);
    
    // Consumer interface - get next segment from queue
    bool ConsumeSegment(StreamSegment& segment);
    
    // Signal end of stream
    void SignalEndOfStream();
    
    // Get queue statistics
    uint64_t GetCapacity() const { return queue_ ? queue_->capacity() : 0; }
    uint64_t GetProducedCount() const { return produced_count_.load(); }
    uint64_t GetConsumedCount() const { return consumed_count_.load(); }
    uint64_t GetDroppedCount() const { return dropped_count_.load(); }
    
    // Check if queue is approximately full (>90% capacity)
    bool IsQueueNearFull() const;
    
    // Check if end of stream was signaled
    bool IsEndOfStream() const { return end_of_stream_.load(); }
    
// Custom deleter for aligned tx_queue_sp_t allocation
struct AlignedTxQueueDeleter {
    void operator()(qcstudio::tx_queue_sp_t* ptr) {
        if (ptr) {
            ptr->~tx_queue_sp_t();  // Call destructor explicitly
#ifdef _WIN32
            _aligned_free(ptr);
#else
            free(ptr);
#endif
        }
    }
};

private:
    std::unique_ptr<qcstudio::tx_queue_sp_t, AlignedTxQueueDeleter> queue_;
    std::atomic<uint64_t> produced_count_{0};
    std::atomic<uint64_t> consumed_count_{0};
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<uint64_t> sequence_counter_{0};
    std::atomic<bool> end_of_stream_{false};
    std::atomic<bool> initialized_{false};
    uint64_t queue_capacity_;
    
    // Helper functions
    bool WriteSegmentToQueue(const StreamSegment& segment);
    bool ReadSegmentFromQueue(StreamSegment& segment);
};

// Named pipe manager for media player communication
class NamedPipeManager {
public:
    explicit NamedPipeManager(const std::wstring& player_path);
    ~NamedPipeManager();
    
    // Create named pipe and launch player
    bool Initialize(const std::wstring& channel_name);
    
    // Write data to player via named pipe
    bool WriteToPlayer(const char* data, size_t size);
    
    // Check if player is still running
    bool IsPlayerRunning() const;
    
    // Get player process handle
    HANDLE GetPlayerProcess() const { return player_process_; }
    
    // Cleanup
    void Cleanup();
    
private:
    std::wstring player_path_;
    HANDLE pipe_handle_;
    HANDLE player_process_;
    PROCESS_INFORMATION process_info_;
    std::wstring pipe_name_;
    bool initialized_;
    bool use_named_pipe_; // True for MPC-HC, false for stdin players
    
    // Helper functions
    std::wstring GenerateUniquePipeName();
    bool CreatePlayerProcess(const std::wstring& channel_name);
    bool CreatePipeWithPlayer();
};

// High-level IPC streaming interface
class TxQueueStreamManager {
public:
    TxQueueStreamManager(const std::wstring& player_path, const std::wstring& channel_name);
    ~TxQueueStreamManager();
    
    // Initialize the streaming system
    bool Initialize();
    
    // Start streaming threads
    bool StartStreaming(
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        std::function<void(const std::wstring&)> log_callback = nullptr,
        std::atomic<int>* chunk_count = nullptr
    );
    
    // Stop streaming
    void StopStreaming();
    
    // Check if streaming is active
    bool IsStreaming() const { return streaming_active_.load(); }
    
    // Get player process handle
    HANDLE GetPlayerProcess() const { return pipe_manager_ ? pipe_manager_->GetPlayerProcess() : nullptr; }
    
    // Get streaming statistics
    struct StreamStats {
        uint64_t segments_produced;
        uint64_t segments_consumed;
        uint64_t segments_dropped;
        uint64_t bytes_transferred;
        bool player_running;
        bool queue_ready;
    };
    StreamStats GetStats() const;
    
private:
    std::wstring player_path_;
    std::wstring channel_name_;
    std::unique_ptr<TxQueueIPC> ipc_manager_;
    std::unique_ptr<NamedPipeManager> pipe_manager_;
    
    std::atomic<bool> streaming_active_{false};
    std::atomic<bool> should_stop_{false};
    std::atomic<uint64_t> bytes_transferred_{0};
    
    std::thread producer_thread_;
    std::thread consumer_thread_;
    
    // Callbacks and state
    std::function<void(const std::wstring&)> log_callback_;
    std::atomic<int>* chunk_count_ptr_;
    std::atomic<bool>* cancel_token_ptr_;
    
    // Thread functions
    void ProducerThreadFunction(const std::wstring& playlist_url);
    void ConsumerThreadFunction();
    
    // Helper functions
    bool DownloadPlaylistSegments(const std::wstring& playlist_url, std::vector<std::wstring>& segment_urls);
    bool DownloadSegment(const std::wstring& segment_url, std::vector<char>& segment_data);
    void LogMessage(const std::wstring& message);
    void UpdateChunkCount(int count);
};

} // namespace tardsplaya