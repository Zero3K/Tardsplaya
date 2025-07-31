#include "http_server.h"
#include <sstream>
#include <iostream>
#include <fstream>

#pragma comment(lib, "ws2_32.lib")

namespace tardsplaya {

HttpStreamServer::HttpStreamServer() {
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        OutputDebugStringA("WSAStartup failed\n");
    }
}

HttpStreamServer::~HttpStreamServer() {
    StopServer();
    WSACleanup();
}

bool HttpStreamServer::StartServer(int port) {
    if (server_running_.load()) {
        return false; // Already running
    }
    
    port_ = port;
    
    // Create socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket_ == INVALID_SOCKET) {
        return false;
    }
    
    // Allow socket reuse
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    // Bind socket
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);
    
    if (bind(server_socket_, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }
    
    // Start listening
    if (listen(server_socket_, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }
    
    server_running_ = true;
    server_thread_ = std::thread(&HttpStreamServer::ServerThread, this);
    
    return true;
}

void HttpStreamServer::StopServer() {
    if (!server_running_.load()) {
        return;
    }
    
    server_running_ = false;
    
    if (server_socket_ != INVALID_SOCKET) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
    }
    
    buffer_cv_.notify_all();
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpStreamServer::AddStreamData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    // Check buffer size limit
    size_t current_size = 0;
    auto temp_queue = stream_buffer_;
    while (!temp_queue.empty()) {
        current_size += temp_queue.front().size();
        temp_queue.pop();
    }
    
    // Remove old data if buffer is too large
    while (current_size + data.size() > MAX_BUFFER_SIZE && !stream_buffer_.empty()) {
        current_size -= stream_buffer_.front().size();
        stream_buffer_.pop();
    }
    
    stream_buffer_.push(data);
    buffer_cv_.notify_all();
}

void HttpStreamServer::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    std::queue<std::vector<uint8_t>> empty;
    stream_buffer_.swap(empty);
}

std::wstring HttpStreamServer::GetStreamUrl() const {
    return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/player.html";
}

void HttpStreamServer::ServerThread() {
    while (server_running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket_, &read_fds);
        
        timeval timeout = {1, 0}; // 1 second timeout
        int result = select(0, &read_fds, nullptr, nullptr, &timeout);
        
        if (result > 0 && FD_ISSET(server_socket_, &read_fds)) {
            SOCKET client_socket = accept(server_socket_, nullptr, nullptr);
            if (client_socket != INVALID_SOCKET) {
                std::thread(&HttpStreamServer::HandleClient, this, client_socket).detach();
            }
        }
    }
}

void HttpStreamServer::HandleClient(SOCKET client_socket) {
    char buffer[4096];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        std::string request(buffer);
        
        // Parse HTTP request
        std::istringstream iss(request);
        std::string method, path, version;
        iss >> method >> path >> version;
        
        if (method == "GET") {
            if (path == "/" || path == "/player.html") {
                SendPlayerHtml(client_socket);
            } else if (path == "/stream.ts") {
                // Send latest stream data - for mpegts.js we need to serve MPEG-TS formatted data
                // For now, return a placeholder response
                std::string response = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: video/mp2t\r\n"
                                     "Access-Control-Allow-Origin: *\r\n"
                                     "Cache-Control: no-cache\r\n"
                                     "Connection: close\r\n"
                                     "\r\n";
                send(client_socket, response.c_str(), response.length(), 0);
                // Note: Real stream data would be sent here by the streaming thread
            } else {
                // 404 Not Found
                std::string not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                send(client_socket, not_found.c_str(), not_found.length(), 0);
            }
        }
    }
    
    closesocket(client_socket);
}

void HttpStreamServer::SendHttpResponse(SOCKET client_socket, const std::string& content_type, const std::vector<uint8_t>& data) {
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << data.size() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Cache-Control: no-cache\r\n";
    response << "\r\n";
    
    std::string header = response.str();
    send(client_socket, header.c_str(), header.length(), 0);
    
    if (!data.empty()) {
        send(client_socket, (char*)data.data(), data.size(), 0);
    }
}

void HttpStreamServer::SendPlayerHtml(SOCKET client_socket) {
    // Split HTML into smaller chunks to avoid C2015 "too many characters in constant" error
    std::string html = R"(<!DOCTYPE html>
<html>
<head>
    <title>Tardsplaya Browser Player</title>
    <meta charset="utf-8">
    <style>
        body { 
            margin: 0; 
            padding: 20px; 
            background: #000; 
            font-family: Arial, sans-serif; 
            color: white;
        }
        #videoContainer { 
            text-align: center; 
            margin: 20px 0;
        }
        video { 
            max-width: 100%; 
            height: auto; 
            background: #000;
        }
        #controls { 
            text-align: center; 
            margin: 20px 0;
        }
        button { 
            padding: 10px 20px; 
            margin: 0 10px; 
            font-size: 16px;
            background: #333;
            color: white;
            border: 1px solid #666;
            cursor: pointer;
        }
        button:hover {
            background: #555;
        }
        #status {
            text-align: center;
            margin: 10px 0;
            font-size: 14px;
            color: #ccc;
        }
    </style>
</head>
<body>
    <h1>Tardsplaya Browser Player</h1>
    <div id="videoContainer">
        <video id="videoPlayer" controls width="800" height="450">
            Your browser does not support the video tag.
        </video>
    </div>
    <div id="controls">
        <button onclick="startPlayback()">Start</button>
        <button onclick="stopPlayback()">Stop</button>
        <button onclick="toggleFullscreen()">Fullscreen</button>
    </div>
    <div id="status">Status: Initializing browser player...</div>

    <script>
        let mediaSource = null;
        let sourceBuffer = null;
        let isPlaying = false;
        let segmentQueue = [];
        let isUpdating = false;
        
        function updateStatus(message) {
            document.getElementById('status').textContent = 'Status: ' + message;
        }
        
        function startPlayback() {
            if (!('MediaSource' in window)) {
                updateStatus('MediaSource API not supported in this browser');
                return;
            }
            
            const videoElement = document.getElementById('videoPlayer');
            
            // Create MediaSource
            mediaSource = new MediaSource();
            videoElement.src = URL.createObjectURL(mediaSource);
            
            mediaSource.addEventListener('sourceopen', function() {
                updateStatus('MediaSource opened, setting up buffer...');
                
                try {
                    // Use MP2T format for MPEG-TS streams)";
    
    html += R"(
                    sourceBuffer = mediaSource.addSourceBuffer('video/mp2t; codecs="avc1.42E01E,mp4a.40.2"');
                    
                    sourceBuffer.addEventListener('updateend', function() {
                        isUpdating = false;
                        // Process next segment in queue
                        processSegmentQueue();
                    });
                    
                    sourceBuffer.addEventListener('error', function(e) {
                        console.error('SourceBuffer error:', e);
                        updateStatus('SourceBuffer error - check console');
                    });
                    
                    updateStatus('Buffer ready, starting stream fetch...');
                    fetchSegments();
                    
                } catch (e) {
                    console.error('Failed to create SourceBuffer:', e);
                    updateStatus('Failed to create SourceBuffer: ' + e.message);
                }
            });
            
            mediaSource.addEventListener('error', function(e) {
                console.error('MediaSource error:', e);
                updateStatus('MediaSource error - check console');
            });
            
            isPlaying = true;
        }
        
        function fetchSegments() {
            if (!isPlaying || !sourceBuffer) return;
            
            fetch('/stream.ts', { 
                method: 'GET',
                cache: 'no-cache'
            })
            .then(response => {
                if (!response.ok) {
                    throw new Error('Network response was not ok');
                }
                return response.arrayBuffer();
            })
            .then(data => {
                if (data.byteLength > 0) {)";
    
    html += R"(
                    segmentQueue.push(new Uint8Array(data));
                    processSegmentQueue();
                    updateStatus('Received segment (' + data.byteLength + ' bytes)');
                } else {
                    updateStatus('No data available, retrying...');
                }
                
                // Continue fetching segments
                if (isPlaying) {
                    setTimeout(fetchSegments, 1000); // Fetch every second
                }
            })
            .catch(error => {
                console.error('Fetch error:', error);
                updateStatus('Fetch error: ' + error.message);
                
                // Retry after error
                if (isPlaying) {
                    setTimeout(fetchSegments, 2000);
                }
            });
        }
        
        function processSegmentQueue() {
            if (isUpdating || segmentQueue.length === 0 || !sourceBuffer) {
                return;
            }
            
            const segment = segmentQueue.shift();
            isUpdating = true;
            
            try {
                sourceBuffer.appendBuffer(segment);
                updateStatus('Processing segment (' + segment.length + ' bytes)');
            } catch (e) {
                console.error('Failed to append buffer:', e);
                updateStatus('Failed to append buffer: ' + e.message);
                isUpdating = false;
            }
        }
        
        function stopPlayback() {)";
    
    html += R"(
            isPlaying = false;
            
            if (mediaSource && mediaSource.readyState === 'open') {
                try {
                    mediaSource.endOfStream();
                } catch (e) {
                    console.warn('Error ending stream:', e);
                }
            }
            
            const videoElement = document.getElementById('videoPlayer');
            videoElement.src = '';
            
            mediaSource = null;
            sourceBuffer = null;
            segmentQueue = [];
            isUpdating = false;
            
            updateStatus('Playback stopped');
        }
        
        function toggleFullscreen() {
            const video = document.getElementById('videoPlayer');
            if (video.requestFullscreen) {
                video.requestFullscreen();
            } else if (video.webkitRequestFullscreen) {
                video.webkitRequestFullscreen();
            } else if (video.msRequestFullscreen) {
                video.msRequestFullscreen();
            }
        }
        
        // Auto-start playback when page loads
        window.addEventListener('load', function() {
            updateStatus('Page loaded, ready to start');
            setTimeout(startPlayback, 1000);
        });
        
        // Handle page unload
        window.addEventListener('beforeunload', function() {
            stopPlayback();
        });
    </script>
</body>
</html>)";
    
    SendHttpResponse(client_socket, "text/html", std::vector<uint8_t>(html.begin(), html.end()));
}

std::string HttpStreamServer::GetMimeType(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".ts") return "video/mp2t";
    return "application/octet-stream";
}

} // namespace tardsplaya