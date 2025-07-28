#pragma once
#include <string>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Forward declarations for Datapath
namespace datapath {
    class iserver;
    class isocket;
    class itask;
    enum class error;
}

/**
 * Datapath-based IPC implementation for high-performance streaming to media players.
 * Replaces the traditional Windows pipe approach with Datapath's optimized IPC.
 * 
 * Features:
 * - High performance, low latency IPC through Datapath
 * - Named pipe interface for media player compatibility
 * - Multiple concurrent stream support
 * - Asynchronous, event-driven architecture
 * - Robust error handling and recovery
 */
class DatapathIPC {
public:
    /**
     * Configuration for Datapath IPC streaming
     */
    struct Config {
        std::wstring channel_name;           // Channel name for stream identification
        std::wstring player_path;            // Path to media player executable
        std::wstring datapath_name;          // Unique Datapath server name
        std::wstring named_pipe_path;        // Named pipe path for media player
        size_t max_buffer_segments = 10;    // Maximum segments to buffer
        size_t segment_timeout_ms = 5000;   // Timeout for segment operations
        size_t connection_timeout_ms = 10000; // Timeout for connections
        bool use_named_pipe_bridge = true;  // Whether to use named pipe bridge
    };

    DatapathIPC();
    ~DatapathIPC();

    /**
     * Initialize the Datapath IPC server with given configuration
     * @param config Configuration parameters
     * @return true on success, false on failure
     */
    bool Initialize(const Config& config);

    /**
     * Start streaming to media player using Datapath IPC
     * @param playlist_url URL of the HLS playlist to stream
     * @param cancel_token Atomic bool for cancellation
     * @param chunk_count Optional pointer to track chunk count
     * @param player_process_handle Optional pointer to store player process handle
     * @return true on success, false on failure
     */
    bool StartStreaming(
        const std::wstring& playlist_url,
        std::atomic<bool>& cancel_token,
        std::atomic<int>* chunk_count = nullptr,
        HANDLE* player_process_handle = nullptr
    );

    /**
     * Stop the streaming and cleanup resources
     */
    void StopStreaming();

    /**
     * Write segment data to the Datapath stream
     * @param data Segment data to write
     * @param cancel_token Cancellation token
     * @return true on success, false on failure
     */
    bool WriteSegmentData(const std::vector<char>& data, std::atomic<bool>& cancel_token);

    /**
     * Check if the IPC system is active and ready
     * @return true if active, false otherwise
     */
    bool IsActive() const;

    /**
     * Get the current buffer size (number of segments queued)
     * @return Number of segments in buffer
     */
    size_t GetBufferSize() const;

    /**
     * Get connection status information
     * @return String describing connection status
     */
    std::wstring GetStatusInfo() const;

    /**
     * Signal end of stream to connected clients
     */
    void SignalEndOfStream();

private:
    // Configuration
    Config config_;

    // Datapath components
    std::shared_ptr<datapath::iserver> datapath_server_;
    std::vector<std::shared_ptr<datapath::isocket>> connected_clients_;
    std::mutex clients_mutex_;

    // Named pipe bridge for media player compatibility
    HANDLE named_pipe_handle_;
    std::thread named_pipe_thread_;
    std::atomic<bool> named_pipe_active_;

    // Streaming state
    std::atomic<bool> is_active_;
    std::atomic<bool> end_of_stream_;
    std::atomic<bool> should_stop_;

    // Data buffering
    std::queue<std::vector<char>> segment_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_condition_;
    std::atomic<size_t> buffer_size_;

    // Worker threads
    std::thread server_thread_;
    std::thread media_player_thread_;
    std::thread buffer_manager_thread_;

    // Process management
    PROCESS_INFORMATION player_process_info_;
    bool player_started_;

    // Internal methods
    bool CreateDatapathServer();
    bool CreateNamedPipeBridge();
    bool LaunchMediaPlayer();
    void ServerThreadProc();
    void NamedPipeThreadProc();
    void BufferManagerThreadProc();
    void MediaPlayerThreadProc();
    void CleanupResources();

    // Event handlers
    void OnClientConnect(bool& accept, std::shared_ptr<datapath::isocket> client);
    void OnClientMessage(std::shared_ptr<datapath::isocket> client, const std::vector<char>& data);
    void OnClientDisconnect(std::shared_ptr<datapath::isocket> client);

    // Utility methods
    std::wstring GenerateDatapathName(const std::wstring& channel) const;
    std::wstring GenerateNamedPipeName(const std::wstring& channel) const;
    bool WriteToDatapathClients(const std::vector<char>& data);
    bool WriteToNamedPipe(const std::vector<char>& data);
};

/**
 * Helper class for creating a Datapath client that reads from server
 * and writes to a named pipe for media player consumption
 */
class DatapathNamedPipeBridge {
public:
    DatapathNamedPipeBridge();
    ~DatapathNamedPipeBridge();

    /**
     * Start the bridge between Datapath client and named pipe
     * @param datapath_name Name of the Datapath server to connect to
     * @param named_pipe_name Name of the named pipe to create
     * @param cancel_token Cancellation token
     * @return true on success, false on failure
     */
    bool Start(
        const std::wstring& datapath_name,
        const std::wstring& named_pipe_name,
        std::atomic<bool>& cancel_token
    );

    /**
     * Stop the bridge and cleanup
     */
    void Stop();

private:
    std::shared_ptr<datapath::isocket> datapath_client_;
    HANDLE named_pipe_handle_;
    std::thread bridge_thread_;
    std::atomic<bool> is_running_;
    std::atomic<bool>* cancel_token_;

    void BridgeThreadProc();
    void OnDataReceived(const std::vector<char>& data);
    void OnConnectionClosed();
};

/**
 * Replacement function for BufferAndPipeStreamToPlayer using Datapath IPC
 * This maintains the same interface as the original function for compatibility
 */
bool BufferAndPipeStreamToPlayerDatapath(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments = 3,
    const std::wstring& channel_name = L"",
    std::atomic<int>* chunk_count = nullptr,
    const std::wstring& selected_quality = L"",
    HANDLE* player_process_handle = nullptr
);