#define NOMINMAX
#include "stream_socket.h"
#include "stream_thread.h"
#include <sstream>
#include <thread>
#include <chrono>

std::atomic<int> StreamSocket::winsock_ref_count_(0);

StreamSocket::StreamSocket() 
    : server_socket_(INVALID_SOCKET)
    , client_socket_(INVALID_SOCKET)
    , port_(0)
    , initialized_(false)
    , client_connected_(false) {
}

StreamSocket::~StreamSocket() {
    Close();
}

bool StreamSocket::Initialize() {
    if (initialized_) return true;
    
    if (!InitializeWinsock()) {
        AddDebugLog(L"StreamSocket::Initialize: Failed to initialize Winsock");
        return false;
    }
    
    // Create TCP socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket_ == INVALID_SOCKET) {
        AddDebugLog(L"StreamSocket::Initialize: Failed to create socket, error=" + 
                   std::to_wstring(WSAGetLastError()));
        return false;
    }
    
    // Find available port
    port_ = FindAvailablePort();
    if (port_ == 0) {
        AddDebugLog(L"StreamSocket::Initialize: Failed to find available port");
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }
    
    // Bind to localhost on the found port
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port_);
    
    if (bind(server_socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        AddDebugLog(L"StreamSocket::Initialize: Failed to bind socket to port " + 
                   std::to_wstring(port_) + L", error=" + std::to_wstring(WSAGetLastError()));
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }
    
    initialized_ = true;
    AddDebugLog(L"StreamSocket::Initialize: Successfully bound to port " + std::to_wstring(port_));
    return true;
}

std::wstring StreamSocket::GetStreamUrl() const {
    if (!initialized_ || port_ == 0) return L"";
    return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/";
}

bool StreamSocket::StartListening() {
    if (!initialized_ || server_socket_ == INVALID_SOCKET) return false;
    
    if (listen(server_socket_, 1) == SOCKET_ERROR) {
        AddDebugLog(L"StreamSocket::StartListening: Failed to listen on socket, error=" + 
                   std::to_wstring(WSAGetLastError()));
        return false;
    }
    
    AddDebugLog(L"StreamSocket::StartListening: Listening on port " + std::to_wstring(port_));
    return true;
}

bool StreamSocket::AcceptConnection(std::atomic<bool>& cancel_token) {
    if (!initialized_ || server_socket_ == INVALID_SOCKET) return false;
    
    // Set socket to non-blocking mode for cancellation support
    u_long mode = 1;
    if (ioctlsocket(server_socket_, FIONBIO, &mode) == SOCKET_ERROR) {
        AddDebugLog(L"StreamSocket::AcceptConnection: Failed to set non-blocking mode, error=" + 
                   std::to_wstring(WSAGetLastError()));
        return false;
    }
    
    AddDebugLog(L"StreamSocket::AcceptConnection: Waiting for client connection on port " + 
               std::to_wstring(port_));
    
    // Wait for connection with cancellation support
    while (!cancel_token.load()) {
        client_socket_ = accept(server_socket_, nullptr, nullptr);
        
        if (client_socket_ != INVALID_SOCKET) {
            // Set client socket back to blocking mode
            mode = 0;
            ioctlsocket(client_socket_, FIONBIO, &mode);
            client_connected_ = true;
            AddDebugLog(L"StreamSocket::AcceptConnection: Client connected on port " + 
                       std::to_wstring(port_));
            return true;
        }
        
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            AddDebugLog(L"StreamSocket::AcceptConnection: Accept failed with error=" + 
                       std::to_wstring(error));
            return false;
        }
        
        // Brief sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    AddDebugLog(L"StreamSocket::AcceptConnection: Cancelled while waiting for connection");
    return false;
}

bool StreamSocket::WriteData(const void* buffer, size_t bufferSize, std::atomic<bool>& cancel_token) {
    if (!client_connected_ || client_socket_ == INVALID_SOCKET) {
        AddDebugLog(L"StreamSocket::WriteData: No client connected");
        return false;
    }
    
    const char* data = static_cast<const char*>(buffer);
    size_t totalSent = 0;
    const size_t MAX_CHUNK_SIZE = 32768; // 32KB chunks
    
    while (totalSent < bufferSize && !cancel_token.load() && client_connected_) {
        size_t chunkSize = (std::min)(bufferSize - totalSent, MAX_CHUNK_SIZE);
        
        int sent = send(client_socket_, data + totalSent, static_cast<int>(chunkSize), 0);
        
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            AddDebugLog(L"StreamSocket::WriteData: Send failed, error=" + std::to_wstring(error));
            
            if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN) {
                // Client disconnected
                client_connected_ = false;
                AddDebugLog(L"StreamSocket::WriteData: Client disconnected");
                return false;
            } else if (error == WSAEWOULDBLOCK) {
                // Would block - retry after a short delay
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            } else {
                // Other error
                return false;
            }
        } else if (sent == 0) {
            // Connection closed gracefully
            client_connected_ = false;
            AddDebugLog(L"StreamSocket::WriteData: Connection closed by client");
            return false;
        }
        
        totalSent += sent;
        
        // Small delay between chunks to prevent overwhelming the client
        if (totalSent < bufferSize) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    return totalSent == bufferSize;
}

bool StreamSocket::IsClientConnected() const {
    return client_connected_ && client_socket_ != INVALID_SOCKET;
}

void StreamSocket::Close() {
    if (client_socket_ != INVALID_SOCKET) {
        closesocket(client_socket_);
        client_socket_ = INVALID_SOCKET;
    }
    
    if (server_socket_ != INVALID_SOCKET) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
    }
    
    client_connected_ = false;
    initialized_ = false;
    
    CleanupWinsock();
}

int StreamSocket::FindAvailablePort() {
    // Try ports from 8000 to 9000
    for (int port = 8000; port < 9000; ++port) {
        SOCKET test_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (test_socket == INVALID_SOCKET) continue;
        
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);
        
        if (bind(test_socket, (sockaddr*)&addr, sizeof(addr)) == 0) {
            closesocket(test_socket);
            return port;
        }
        
        closesocket(test_socket);
    }
    
    return 0; // No available port found
}

bool StreamSocket::InitializeWinsock() {
    if (winsock_ref_count_.fetch_add(1) == 0) {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            AddDebugLog(L"StreamSocket::InitializeWinsock: WSAStartup failed with error=" + 
                       std::to_wstring(result));
            winsock_ref_count_.fetch_sub(1);
            return false;
        }
        AddDebugLog(L"StreamSocket::InitializeWinsock: Winsock initialized");
    }
    return true;
}

void StreamSocket::CleanupWinsock() {
    if (winsock_ref_count_.fetch_sub(1) == 1) {
        WSACleanup();
        AddDebugLog(L"StreamSocket::CleanupWinsock: Winsock cleaned up");
    }
}