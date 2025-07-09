#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <atomic>

// Socket-based streaming replacement for pipe-based communication
// SOLUTION: This replaces Windows named pipes with TCP sockets to fix multi-stream issues.
// Named pipes were causing "Pipe write failed" errors when multiple streams ran simultaneously.
// TCP sockets provide better reliability, flow control, and error handling for multiple concurrent streams.
class StreamSocket {
public:
    StreamSocket();
    ~StreamSocket();
    
    // Initialize socket and bind to a random available port
    bool Initialize();
    
    // Get the port number that was bound
    int GetPort() const { return port_; }
    
    // Get the URL that media players should connect to
    std::wstring GetStreamUrl() const;
    
    // Start listening for connections
    bool StartListening();
    
    // Accept a connection from media player (blocking)
    bool AcceptConnection(std::atomic<bool>& cancel_token);
    
    // Write data to the connected client
    bool WriteData(const void* buffer, size_t bufferSize, std::atomic<bool>& cancel_token);
    
    // Check if client is still connected
    bool IsClientConnected() const;
    
    // Close the connection and cleanup
    void Close();
    
private:
    SOCKET server_socket_;
    SOCKET client_socket_;
    int port_;
    bool initialized_;
    bool client_connected_;
    
    // Find an available port
    int FindAvailablePort();
    
    // Initialize Winsock
    static bool InitializeWinsock();
    static void CleanupWinsock();
    static std::atomic<int> winsock_ref_count_;
};