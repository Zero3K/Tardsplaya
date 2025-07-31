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
        OutputDebugStringA(("WSAStartup failed with error: " + std::to_string(result)).c_str());
    } else {
        OutputDebugStringA("WSAStartup successful");
    }
}

HttpStreamServer::~HttpStreamServer() {
    StopServer();
    WSACleanup();
}

bool HttpStreamServer::StartServer(int port) {
    if (server_running_.load()) {
        OutputDebugStringA("HTTP server already running");
        return false; // Already running
    }
    
    port_ = port;
    
    OutputDebugStringA(("Starting HTTP server on port " + std::to_string(port)).c_str());
    
    // Create socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket_ == INVALID_SOCKET) {
        int error = WSAGetLastError();
        OutputDebugStringA(("Failed to create server socket, error: " + std::to_string(error)).c_str());
        return false;
    }
    
    // Allow socket reuse
    int opt = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        OutputDebugStringA(("Failed to set SO_REUSEADDR, error: " + std::to_string(error)).c_str());
    }
    
    // Bind socket
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // Bind specifically to localhost
    server_addr.sin_port = htons(port_);
    
    if (bind(server_socket_, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        OutputDebugStringA(("Failed to bind to port " + std::to_string(port_) + ", error: " + std::to_string(error)).c_str());
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }
    
    // Start listening
    if (listen(server_socket_, SOMAXCONN) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        OutputDebugStringA(("Failed to start listening on socket, error: " + std::to_string(error)).c_str());
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }
    
    OutputDebugStringA(("HTTP server listening on 127.0.0.1:" + std::to_string(port_)).c_str());
    
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
    
    // Debug log
    OutputDebugStringA(("Adding stream data: " + std::to_string(data.size()) + " bytes").c_str());
    
    // Check buffer size limit
    size_t current_size = 0;
    auto temp_queue = stream_buffer_;
    while (!temp_queue.empty()) {
        current_size += temp_queue.front().size();
        temp_queue.pop();
    }
    
    OutputDebugStringA(("Buffer size before adding: " + std::to_string(stream_buffer_.size()) + " segments, " + std::to_string(current_size) + " bytes").c_str());
    
    // Remove old data if buffer is too large
    while (current_size + data.size() > MAX_BUFFER_SIZE && !stream_buffer_.empty()) {
        size_t removed_size = stream_buffer_.front().size();
        current_size -= removed_size;
        stream_buffer_.pop();
        OutputDebugStringA(("Removed old segment: " + std::to_string(removed_size) + " bytes").c_str());
    }
    
    stream_buffer_.push(data);
    OutputDebugStringA(("Buffer size after adding: " + std::to_string(stream_buffer_.size()) + " segments").c_str());
    
    // Mark buffer as ready when we have enough segments
    if (stream_buffer_.size() >= MIN_BUFFER_SEGMENTS && !buffer_ready_.load()) {
        buffer_ready_.store(true);
        OutputDebugStringA(("Buffer ready! Have " + std::to_string(stream_buffer_.size()) + " segments (min " + std::to_string(MIN_BUFFER_SEGMENTS) + ")").c_str());
    }
    
    buffer_cv_.notify_all();
}

void HttpStreamServer::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    std::queue<std::vector<uint8_t>> empty;
    stream_buffer_.swap(empty);
    buffer_ready_.store(false);
    OutputDebugStringA("Buffer cleared and reset to not ready");
}

std::wstring HttpStreamServer::GetStreamUrl() const {
    return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/player.html";
}

void HttpStreamServer::ServerThread() {
    OutputDebugStringA("HTTP Server thread started, entering main loop");
    
    while (server_running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket_, &read_fds);
        
        timeval timeout = {1, 0}; // 1 second timeout
        int result = select(0, &read_fds, nullptr, nullptr, &timeout);
        
        if (result > 0 && FD_ISSET(server_socket_, &read_fds)) {
            sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            SOCKET client_socket = accept(server_socket_, (sockaddr*)&client_addr, &addr_len);
            if (client_socket != INVALID_SOCKET) {
                OutputDebugStringA("Accepted new client connection");
                std::thread(&HttpStreamServer::HandleClient, this, client_socket).detach();
            } else {
                int error = WSAGetLastError();
                OutputDebugStringA(("Accept failed with error: " + std::to_string(error)).c_str());
            }
        } else if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            OutputDebugStringA(("Select failed with error: " + std::to_string(error)).c_str());
        }
    }
    
    OutputDebugStringA("HTTP Server thread exiting");
}

void HttpStreamServer::HandleClient(SOCKET client_socket) {
    OutputDebugStringA("HandleClient: Starting to handle new client");
    
    char buffer[4096];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        std::string request(buffer);
        
        OutputDebugStringA(("HandleClient: Received " + std::to_string(bytes_received) + " bytes").c_str());
        
        // Parse HTTP request
        std::istringstream iss(request);
        std::string method, path, version;
        iss >> method >> path >> version;
        
        // Debug logging for HTTP requests
        OutputDebugStringA(("HTTP Request: " + method + " " + path).c_str());
        
        if (method == "GET") {
            if (path == "/" || path == "/player.html") {
                OutputDebugStringA("Serving player HTML");
                SendPlayerHtml(client_socket);
            } else if (path.find("/stream.ts") == 0) {
                // Serve actual buffered stream data
                OutputDebugStringA("Serving stream data");
                ServeStreamData(client_socket);
            } else if (path == "/player.js") {
                // Serve the JavaScript file
                OutputDebugStringA("Serving player.js");
                ServePlayerJs(client_socket);
            } else if (path == "/ping") {
                // Simple connectivity test endpoint
                OutputDebugStringA("Serving ping response");
                std::string ping_response = "HTTP/1.1 200 OK\r\n"
                                          "Content-Type: text/plain\r\n"
                                          "Content-Length: 4\r\n"
                                          "Access-Control-Allow-Origin: *\r\n"
                                          "Connection: close\r\n"
                                          "\r\n"
                                          "pong";
                int bytes_sent = send(client_socket, ping_response.c_str(), ping_response.length(), 0);
                OutputDebugStringA(("Ping response sent: " + std::to_string(bytes_sent) + " bytes").c_str());
            } else {
                // 404 Not Found
                OutputDebugStringA(("404 Not Found: " + path).c_str());
                std::string not_found = "HTTP/1.1 404 Not Found\r\n"
                                      "Content-Type: text/plain\r\n"
                                      "Content-Length: 13\r\n"
                                      "Access-Control-Allow-Origin: *\r\n"
                                      "Connection: close\r\n"
                                      "\r\n"
                                      "File not found";
                send(client_socket, not_found.c_str(), not_found.length(), 0);
            }
        } else if (method == "OPTIONS") {
            // Handle CORS preflight requests
            OutputDebugStringA("Handling CORS preflight request");
            std::string cors_response = "HTTP/1.1 200 OK\r\n"
                                      "Access-Control-Allow-Origin: *\r\n"
                                      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                                      "Access-Control-Allow-Headers: Content-Type\r\n"
                                      "Access-Control-Max-Age: 86400\r\n"
                                      "Content-Length: 0\r\n"
                                      "Connection: close\r\n"
                                      "\r\n";
            send(client_socket, cors_response.c_str(), cors_response.length(), 0);
        }
    } else if (bytes_received == 0) {
        OutputDebugStringA("HandleClient: Client closed connection gracefully");
    } else {
        int error = WSAGetLastError();
        OutputDebugStringA(("HandleClient: recv failed with error: " + std::to_string(error)).c_str());
    }
    
    OutputDebugStringA("HandleClient: Closing client socket");
    closesocket(client_socket);
}

void HttpStreamServer::SendHttpResponse(SOCKET client_socket, const std::string& content_type, const std::vector<uint8_t>& data) {
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << data.size() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Cache-Control: no-cache\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    
    std::string header = response.str();
    int header_sent = send(client_socket, header.c_str(), header.length(), 0);
    OutputDebugStringA(("SendHttpResponse: Header sent: " + std::to_string(header_sent) + " bytes").c_str());
    
    if (!data.empty()) {
        int data_sent = send(client_socket, (char*)data.data(), data.size(), 0);
        OutputDebugStringA(("SendHttpResponse: Data sent: " + std::to_string(data_sent) + " bytes").c_str());
    }
}

void HttpStreamServer::SendPlayerHtml(SOCKET client_socket) {
    std::string html = "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <title>Tardsplaya Browser Player</title>\n"
        "    <meta charset=\"utf-8\">\n"
        "    <style>\n"
        "        body { \n"
        "            margin: 0; \n"
        "            padding: 20px; \n"
        "            background: #000; \n"
        "            font-family: Arial, sans-serif; \n"
        "            color: white;\n"
        "        }\n"
        "        #videoContainer { \n"
        "            text-align: center; \n"
        "            margin: 20px 0;\n"
        "        }\n"
        "        video { \n"
        "            max-width: 100%; \n"
        "            height: auto; \n"
        "            background: #000;\n"
        "        }\n"
        "        #controls { \n"
        "            text-align: center; \n"
        "            margin: 20px 0;\n"
        "        }\n"
        "        button { \n"
        "            padding: 10px 20px; \n"
        "            margin: 0 10px; \n"
        "            font-size: 16px;\n"
        "            background: #333;\n"
        "            color: white;\n"
        "            border: 1px solid #666;\n"
        "            cursor: pointer;\n"
        "        }\n"
        "        button:hover {\n"
        "            background: #555;\n"
        "        }\n"
        "        #status {\n"
        "            text-align: center;\n"
        "            margin: 10px 0;\n"
        "            font-size: 14px;\n"
        "            color: #ccc;\n"
        "        }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <h1>Tardsplaya Browser Player</h1>\n"
        "    <div id=\"videoContainer\">\n"
        "        <video id=\"videoPlayer\" controls width=\"800\" height=\"450\">\n"
        "            Your browser does not support the video tag.\n"
        "        </video>\n"
        "    </div>\n"
        "    <div id=\"controls\">\n"
        "        <button onclick=\"startPlayback()\">Start</button>\n"
        "        <button onclick=\"stopPlayback()\">Stop</button>\n"
        "        <button onclick=\"toggleFullscreen()\">Fullscreen</button>\n"
        "    </div>\n"
        "    <div id=\"status\">Status: Initializing browser player...</div>\n"
        "\n"
        "    <script src=\"/player.js\"></script>\n"
        "</body>\n"
        "</html>";
    
    SendHttpResponse(client_socket, "text/html", std::vector<uint8_t>(html.begin(), html.end()));
}

std::string HttpStreamServer::GetMimeType(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".ts") return "video/mp2t";
    return "application/octet-stream";
}

void HttpStreamServer::ServeStreamData(SOCKET client_socket) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    OutputDebugStringA(("ServeStreamData: Buffer has " + std::to_string(stream_buffer_.size()) + " segments, ready=" + (buffer_ready_.load() ? "true" : "false")).c_str());
    
    // Only serve data if buffer is ready (has enough segments)
    if (buffer_ready_.load() && !stream_buffer_.empty()) {
        std::vector<uint8_t> segment_data = stream_buffer_.front();
        stream_buffer_.pop();
        
        // Debug log
        OutputDebugStringA(("Serving segment data: " + std::to_string(segment_data.size()) + " bytes, " + std::to_string(stream_buffer_.size()) + " segments remaining").c_str());
        
        // Send HTTP response with the segment data
        SendHttpResponse(client_socket, "video/mp2t", segment_data);
        
        // Update bytes served counter
        total_bytes_served_ += segment_data.size();
        OutputDebugStringA(("Total bytes served: " + std::to_string(total_bytes_served_.load())).c_str());
    } else {
        // No data available or buffer not ready, send empty response
        std::string reason = buffer_ready_.load() ? "buffer empty" : "buffer not ready (need " + std::to_string(MIN_BUFFER_SEGMENTS) + " segments)";
        OutputDebugStringA(("No stream data available (" + reason + "), sending 204 No Content").c_str());
        
        std::string response = "HTTP/1.1 204 No Content\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "Cache-Control: no-cache\r\n"
                             "Connection: close\r\n"
                             "\r\n";
        int bytes_sent = send(client_socket, response.c_str(), response.length(), 0);
        OutputDebugStringA(("204 response sent: " + std::to_string(bytes_sent) + " bytes").c_str());
    }
}

void HttpStreamServer::ServeStaticFile(SOCKET client_socket, const std::string& filename) {
    // Debug logging for file serving
    OutputDebugStringA(("Serving static file: " + filename).c_str());
    
    // Try to read the file from the current directory
    std::ifstream file(filename, std::ios::binary);
    std::string tried_path = filename;
    
    if (!file.is_open()) {
        // Also try with executable directory (in case we're in a different working directory)
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(NULL, exe_path, MAX_PATH);
        std::wstring exe_dir(exe_path);
        size_t last_slash = exe_dir.find_last_of(L"\\/");
        if (last_slash != std::wstring::npos) {
            exe_dir = exe_dir.substr(0, last_slash + 1);
            std::wstring full_path = exe_dir + std::wstring(filename.begin(), filename.end());
            std::string full_path_str(full_path.begin(), full_path.end());
            tried_path = full_path_str;
            file.open(full_path_str, std::ios::binary);
            
            OutputDebugStringA(("Tried executable directory path: " + full_path_str).c_str());
        }
        
        if (!file.is_open()) {
            // File not found - log the error
            OutputDebugStringA(("File not found: " + tried_path).c_str());
            
            // Send 404 response
            std::string not_found = "HTTP/1.1 404 Not Found\r\n"
                                  "Content-Type: text/plain\r\n"
                                  "Content-Length: 13\r\n"
                                  "Access-Control-Allow-Origin: *\r\n"
                                  "\r\n"
                                  "File not found";
            send(client_socket, not_found.c_str(), not_found.length(), 0);
            return;
        }
    }
    
    // Read file contents
    std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    file.close();
    
    OutputDebugStringA(("Successfully loaded file, size: " + std::to_string(file_data.size()) + " bytes").c_str());
    
    // Send HTTP response
    std::string content_type = GetMimeType(filename);
    SendHttpResponse(client_socket, content_type, file_data);
}

void HttpStreamServer::ServePlayerJs(SOCKET client_socket) {
    // Embedded player.js content - this ensures it's always available regardless of working directory
    const std::string player_js_content = R"(// Tardsplaya Browser Player - MediaSource API Implementation
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
            // Use MP2T format for MPEG-TS streams
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
            
            updateStatus('Buffer ready, testing connectivity...');
            testConnectivity();
            
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

function testConnectivity() {
    updateStatus('Testing server connectivity...');
    
    fetch('/ping', { 
        method: 'GET',
        cache: 'no-cache',
        headers: {
            'Accept': 'text/plain'
        }
    })
    .then(response => {
        console.log('Ping response status:', response.status, 'statusText:', response.statusText);
        console.log('Ping response headers:', Array.from(response.headers.entries()));
        
        if (response.ok) {
            return response.text();
        } else {
            throw new Error('Server responded with status: ' + response.status + ' ' + response.statusText);
        }
    })
    .then(text => {
        console.log('Ping response text:', text);
        updateStatus('Server connectivity OK (received: ' + text + '), waiting for buffer to build...');
        
        // Add a delay to allow the server to buffer some segments before we start requesting
        setTimeout(() => {
            updateStatus('Starting stream fetch...');
            fetchSegments();
        }, 3000); // Wait 3 seconds for buffering
    })
    .catch(error => {
        console.error('Connectivity test failed:', error);
        console.error('Error details:', {
            name: error.name,
            message: error.message,
            stack: error.stack
        });
        updateStatus('Server connectivity failed: ' + error.message + ' (type: ' + error.constructor.name + ')');
        // Still try to fetch segments after delay
        setTimeout(() => {
            updateStatus('Retrying stream fetch despite connectivity test failure...');
            fetchSegments();
        }, 2000);
    });
}

function fetchSegments() {
    if (!isPlaying || !sourceBuffer) {
        console.log('fetchSegments: Aborted - isPlaying:', isPlaying, 'sourceBuffer:', !!sourceBuffer);
        return;
    }
    
    console.log('fetchSegments: Starting fetch request to /stream.ts');
    updateStatus('Requesting stream data...');
    
    fetch('/stream.ts', { 
        method: 'GET',
        cache: 'no-cache',
        headers: {
            'Accept': 'video/mp2t'
        }
    })
    .then(response => {
        console.log('Fetch response status:', response.status, 'statusText:', response.statusText);
        console.log('Fetch response headers:', Array.from(response.headers.entries()));
        console.log('Fetch response type:', response.type);
        console.log('Fetch response url:', response.url);
        
        if (response.status === 204) {
            // No content available, retry after delay
            updateStatus('No data available (status 204), retrying...');
            if (isPlaying) {
                setTimeout(fetchSegments, 1000);
            }
            return null;
        } else if (!response.ok) {
            throw new Error('Network response was not ok: ' + response.status + ' ' + response.statusText);
        }
        
        updateStatus('Received response (status ' + response.status + '), processing data...');
        return response.arrayBuffer();
    })
    .then(data => {
        if (data && data.byteLength > 0) {
            console.log('Received segment data:', data.byteLength, 'bytes');
            segmentQueue.push(new Uint8Array(data));
            processSegmentQueue();
            updateStatus('Received segment (' + data.byteLength + ' bytes), queue length: ' + segmentQueue.length);
        } else {
            console.log('No data received or empty response');
        }
        
        // Continue fetching segments with appropriate delay
        if (isPlaying) {
            // Use shorter delay when data is available, longer when no data
            const delay = (data && data.byteLength > 0) ? 500 : 1500;
            setTimeout(fetchSegments, delay);
        }
    })
    .catch(error => {
        console.error('Fetch error details:', error);
        console.error('Error type:', error.constructor.name);
        console.error('Error message:', error.message);
        console.error('Error stack:', error.stack);
        
        // Check for specific error types
        let errorDetail = '';
        if (error instanceof TypeError) {
            errorDetail = ' (likely network/connectivity issue)';
        } else if (error.name === 'AbortError') {
            errorDetail = ' (request was aborted)';
        }
        
        updateStatus('Fetch error: ' + error.message + ' (type: ' + error.constructor.name + ')' + errorDetail);
        
        // Retry after error with longer delay
        if (isPlaying) {
            console.log('Scheduling retry in 3 seconds...');
            setTimeout(fetchSegments, 3000);
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

function stopPlayback() {
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
});)";

    std::vector<uint8_t> js_data(player_js_content.begin(), player_js_content.end());
    SendHttpResponse(client_socket, "application/javascript", js_data);
}

} // namespace tardsplaya