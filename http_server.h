#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace tardsplaya {

// Simple HTTP server for streaming video data to browsers
class HttpStreamServer {
public:
    HttpStreamServer();
    ~HttpStreamServer();

    // Start the HTTP server on specified port
    bool StartServer(int port = 8080);
    
    // Stop the HTTP server
    void StopServer();
    
    // Add stream data to be served
    void AddStreamData(const std::vector<uint8_t>& data);
    
    // Clear stream buffer
    void ClearBuffer();
    
    // Get the server URL for browser access
    std::wstring GetStreamUrl() const;
    
    // Check if server is running
    bool IsRunning() const { return server_running_.load(); }
    
    // Get current port
    int GetPort() const { return port_; }

private:
    void ServerThread();
    void HandleClient(SOCKET client_socket);
    void SendHttpResponse(SOCKET client_socket, const std::string& content_type, const std::vector<uint8_t>& data);
    void SendPlayerHtml(SOCKET client_socket);
    std::string GetMimeType(const std::string& path);

    std::atomic<bool> server_running_{false};
    std::thread server_thread_;
    SOCKET server_socket_ = INVALID_SOCKET;
    int port_ = 8080;
    
    // Stream data buffer
    std::queue<std::vector<uint8_t>> stream_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    std::atomic<size_t> total_bytes_served_{0};
    
    // Maximum buffer size (10MB)
    static const size_t MAX_BUFFER_SIZE = 10 * 1024 * 1024;
};

} // namespace tardsplaya