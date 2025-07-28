#include "datapath_ipc.h"
#include "stream_thread.h"
#include "playlist_parser.h"
#include "twitch_api.h"
#include "tsduck_hls_wrapper.h"

// Include Datapath headers
#include "datapath/include/datapath.hpp"
#include "datapath/include/error.hpp"
#include "datapath/include/permissions.hpp"

#include <sstream>
#include <algorithm>
#include <chrono>
#include <set>
#include <map>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// Local implementations of HTTP and parsing functions
static bool HttpGetBinaryLocal(const std::wstring& url, std::vector<char>& out, int max_attempts = 3, std::atomic<bool>* cancel_token = nullptr) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (cancel_token && cancel_token->load()) return false;
        URL_COMPONENTS uc = { sizeof(uc) };
        wchar_t host[256] = L"", path[2048] = L"";
        uc.lpszHostName = host; uc.dwHostNameLength = 255;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
        WinHttpCrackUrl(url.c_str(), 0, 0, &uc);

        HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
        if (!hSession) continue;
        HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); continue; }
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        BOOL res = WinHttpSendRequest(hRequest, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hRequest, 0);
        if (!res) { 
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); 
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            continue; 
        }

        DWORD dwSize = 0;
        out.clear();
        bool error = false;
        do {
            if (cancel_token && cancel_token->load()) { error = true; break; }
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            size_t prev_size = out.size();
            out.resize(prev_size + dwSize);
            if (!WinHttpReadData(hRequest, out.data() + prev_size, dwSize, &dwDownloaded) || dwDownloaded == 0)
            {
                error = true;
                break;
            }
            if (dwDownloaded < dwSize) out.resize(prev_size + dwDownloaded);
        } while (dwSize > 0);

        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        if (!error && !out.empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
    return false;
}

static bool HttpGetTextLocal(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token) {
    std::vector<char> data;
    if (!HttpGetBinaryLocal(url, data, 3, cancel_token)) return false;
    out.assign(data.begin(), data.end());
    return true;
}

static std::wstring JoinUrlLocal(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}

struct SegmentInfo {
    std::wstring url;
    double sequence_number;
    bool has_discontinuity;
    std::chrono::milliseconds duration;
};

static std::pair<std::vector<SegmentInfo>, bool> ParseSegmentsWithSequencing(const std::string& playlist) {
    std::vector<SegmentInfo> segments;
    bool should_clear_buffer = false;
    
    // Use the proper HLS parser to handle sequencing
    tsduck_hls::PlaylistParser parser;
    if (!parser.ParsePlaylist(playlist)) {
        AddDebugLog(L"DatapathIPC::ParseSegmentsWithSequencing: Failed to parse playlist with HLS parser");
        return std::make_pair(segments, false);
    }
    
    auto hls_segments = parser.GetSegments();
    int64_t media_sequence = parser.GetMediaSequence();
    bool has_discontinuities = parser.HasDiscontinuities();
    
    AddDebugLog(L"DatapathIPC::ParseSegmentsWithSequencing: Parsed " + std::to_wstring(hls_segments.size()) + 
               L" segments, media_sequence=" + std::to_wstring(media_sequence) +
               L", discontinuities=" + (has_discontinuities ? L"true" : L"false"));
    
    // If there are discontinuities, we should clear the buffer to avoid mixing old/new content
    if (has_discontinuities) {
        should_clear_buffer = true;
        AddDebugLog(L"DatapathIPC::ParseSegmentsWithSequencing: Discontinuity detected - will clear buffer");
    }
    
    // Convert to our segment info format with sequence tracking
    for (size_t i = 0; i < hls_segments.size(); ++i) {
        const auto& hls_seg = hls_segments[i];
        SegmentInfo seg_info;
        seg_info.url = hls_seg.url;
        seg_info.sequence_number = media_sequence + i; // Calculate proper sequence number
        seg_info.has_discontinuity = hls_seg.has_discontinuity;
        seg_info.duration = hls_seg.duration;
        segments.push_back(seg_info);
    }
    
    return std::make_pair(segments, should_clear_buffer);
}

DatapathIPC::DatapathIPC()
    : named_pipe_handle_(INVALID_HANDLE_VALUE)
    , stdin_read_handle_(INVALID_HANDLE_VALUE)
    , named_pipe_active_(false)
    , is_active_(false)
    , end_of_stream_(false)
    , should_stop_(false)
    , buffer_size_(0)
    , player_started_(false)
    , next_expected_sequence_(0.0)
    , last_processed_sequence_(-1.0)
{
    AddDebugLog(L"[DATAPATH] DatapathIPC constructor called");
    memset(&player_process_info_, 0, sizeof(player_process_info_));
}

DatapathIPC::~DatapathIPC() {
    StopStreaming();
}

bool DatapathIPC::Initialize(const Config& config) {
    AddDebugLog(L"[DATAPATH] DatapathIPC::Initialize: Entry point reached");
    
    if (is_active_.load()) {
        AddDebugLog(L"[DATAPATH] DatapathIPC::Initialize: Already initialized");
        return false;
    }

    AddDebugLog(L"[DATAPATH] DatapathIPC::Initialize: Setting up configuration");
    config_ = config;
    
    // Generate unique names if not provided
    if (config_.datapath_name.empty()) {
        config_.datapath_name = GenerateDatapathName(config_.channel_name);
        AddDebugLog(L"[DATAPATH] Generated datapath name: " + config_.datapath_name);
    }

    AddDebugLog(L"[DATAPATH] DatapathIPC::Initialize: Initializing for channel " + config_.channel_name +
               L", datapath_name=" + config_.datapath_name +
               L", using stdin pipe for media player");

    // Create Datapath server
    AddDebugLog(L"[DATAPATH] Attempting to create Datapath server...");
    if (!CreateDatapathServer()) {
        AddDebugLog(L"[DATAPATH] ERROR: DatapathIPC::Initialize: Failed to create Datapath server");
        return false;
    }
    AddDebugLog(L"[DATAPATH] Datapath server created successfully");

    // Create named pipe bridge if enabled
    if (config_.use_named_pipe_bridge) {
        AddDebugLog(L"[DATAPATH] Attempting to create named pipe bridge...");
        if (!CreateNamedPipeBridge()) {
            AddDebugLog(L"[DATAPATH] ERROR: DatapathIPC::Initialize: Failed to create named pipe bridge");
            CleanupResources();
            return false;
        }
        AddDebugLog(L"[DATAPATH] Named pipe bridge created successfully");
    } else {
        AddDebugLog(L"[DATAPATH] Named pipe bridge disabled by configuration");
    }

    is_active_.store(true);
    AddDebugLog(L"[DATAPATH] DatapathIPC::Initialize: Successfully initialized for " + config_.channel_name);
    return true;
}

bool DatapathIPC::StartStreaming(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::atomic<int>* chunk_count,
    HANDLE* player_process_handle
) {
    if (!is_active_.load()) {
        AddDebugLog(L"DatapathIPC::StartStreaming: Not initialized");
        return false;
    }

    AddDebugLog(L"DatapathIPC::StartStreaming: Starting stream for " + config_.channel_name +
               L", URL=" + playlist_url);

    // Start worker threads first
    should_stop_.store(false);
    end_of_stream_.store(false);

    server_thread_ = std::thread(&DatapathIPC::ServerThreadProc, this);
    buffer_manager_thread_ = std::thread(&DatapathIPC::BufferManagerThreadProc, this);

    // Start named pipe thread BEFORE launching media player to ensure pipe is ready
    if (config_.use_named_pipe_bridge) {
        AddDebugLog(L"DatapathIPC::StartStreaming: Starting stdin pipe thread before media player");
        named_pipe_thread_ = std::thread(&DatapathIPC::NamedPipeThreadProc, this);
        
        // Give the stdin pipe thread time to start and be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        AddDebugLog(L"DatapathIPC::StartStreaming: Stdin pipe thread started, now launching media player");
    }

    // Launch media player after named pipe is ready
    if (!LaunchMediaPlayer()) {
        AddDebugLog(L"DatapathIPC::StartStreaming: Failed to launch media player");
        return false;
    }

    // Give the media player time to start up and be ready to receive stdin data
    AddDebugLog(L"DatapathIPC::StartStreaming: Giving media player 500ms to start up...");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (player_process_handle) {
        *player_process_handle = player_process_info_.hProcess;
    }

    media_player_thread_ = std::thread(&DatapathIPC::MediaPlayerThreadProc, this);

    // Main streaming loop - download and buffer segments with proper sequencing
    std::set<std::wstring> seen_urls;
    int consecutive_errors = 0;
    const int max_consecutive_errors = 15;
    bool sequence_initialized = false;

    while (!cancel_token.load() && !should_stop_.load() && consecutive_errors < max_consecutive_errors) {
        // Download playlist
        std::string playlist;
        if (!HttpGetTextLocal(playlist_url, playlist, &cancel_token)) {
            consecutive_errors++;
            AddDebugLog(L"DatapathIPC::StartStreaming: Failed to download playlist, error " + 
                       std::to_wstring(consecutive_errors) + L"/" + std::to_wstring(max_consecutive_errors));
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        consecutive_errors = 0;

        // Check for stream end
        if (playlist.find("#EXT-X-ENDLIST") != std::string::npos) {
            AddDebugLog(L"DatapathIPC::StartStreaming: Found #EXT-X-ENDLIST - stream ended");
            end_of_stream_.store(true);
            break;
        }

        // Parse segments with proper HLS sequencing
        auto parse_result = ParseSegmentsWithSequencing(playlist);
        auto segments = parse_result.first;
        bool should_clear_buffer = parse_result.second;

        AddDebugLog(L"DatapathIPC::StartStreaming: Parsed " + std::to_wstring(segments.size()) + 
                   L" segments from playlist, should_clear_buffer=" + (should_clear_buffer ? L"true" : L"false"));

        // Initialize sequence tracking on first playlist parse or discontinuity
        if (!sequence_initialized || should_clear_buffer) {
            if (!segments.empty()) {
                std::lock_guard<std::mutex> lock(sequence_mutex_);
                next_expected_sequence_ = segments[0].sequence_number;
                last_processed_sequence_ = segments[0].sequence_number - 1;
                sequence_initialized = true;
                AddDebugLog(L"DatapathIPC::StartStreaming: Initialized sequence tracking - next_expected=" + 
                           std::to_wstring(next_expected_sequence_) + L", last_processed=" + 
                           std::to_wstring(last_processed_sequence_));
            }
        }

        // Handle discontinuities by clearing buffers
        if (should_clear_buffer) {
            AddDebugLog(L"DatapathIPC::StartStreaming: Clearing buffers due to discontinuity");
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                while (!segment_buffer_.empty()) {
                    segment_buffer_.pop();
                }
                buffer_size_.store(0);
            }
            {
                std::lock_guard<std::mutex> lock(sequence_mutex_);
                sequence_ordered_segments_.clear();
                seen_urls.clear();
                // Note: sequence tracking was already reset above in the initialization block
            }
        }

        // Download new segments in sequence order
        for (const auto& seg : segments) {
            if (cancel_token.load() || should_stop_.load()) break;

            // Skip segments we've already seen
            if (seen_urls.count(seg.url)) {
                AddDebugLog(L"DatapathIPC::StartStreaming: Skipping already seen segment sequence=" + 
                           std::to_wstring(seg.sequence_number));
                continue;
            }

            // Skip segments that are too old (already processed)
            if (seg.sequence_number <= last_processed_sequence_) {
                AddDebugLog(L"DatapathIPC::StartStreaming: Skipping old segment sequence=" + 
                           std::to_wstring(seg.sequence_number) + L", last_processed=" + 
                           std::to_wstring(last_processed_sequence_));
                seen_urls.insert(seg.url);
                continue;
            }

            // Check buffer size
            if (buffer_size_.load() >= config_.max_buffer_segments) {
                AddDebugLog(L"DatapathIPC::StartStreaming: Buffer full, waiting");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            seen_urls.insert(seg.url);
            std::wstring seg_url = JoinUrlLocal(playlist_url, seg.url);
            std::vector<char> seg_data;

            AddDebugLog(L"DatapathIPC::StartStreaming: Downloading segment sequence=" + 
                       std::to_wstring(seg.sequence_number) + L", URL=" + seg.url);

            // Download segment with retries
            bool download_ok = false;
            for (int retry = 0; retry < 3; ++retry) {
                if (HttpGetBinaryLocal(seg_url, seg_data, 1, &cancel_token)) {
                    download_ok = true;
                    break;
                }
                if (cancel_token.load() || should_stop_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }

            if (download_ok && !seg_data.empty()) {
                // Store segment in sequence-ordered buffer
                {
                    std::lock_guard<std::mutex> lock(sequence_mutex_);
                    sequence_ordered_segments_[seg.sequence_number] = std::move(seg_data);
                    AddDebugLog(L"DatapathIPC::StartStreaming: Stored segment sequence=" + 
                               std::to_wstring(seg.sequence_number) + L" in sequence buffer");
                }

                // Try to release segments in order to the playback buffer
                ProcessSequencedSegments();

                if (chunk_count) {
                    chunk_count->store(static_cast<int>(buffer_size_.load()));
                }

                AddDebugLog(L"DatapathIPC::StartStreaming: Downloaded segment sequence=" + 
                           std::to_wstring(seg.sequence_number) + L", buffer=" + 
                           std::to_wstring(buffer_size_.load()));
            } else {
                AddDebugLog(L"DatapathIPC::StartStreaming: Failed to download segment sequence=" + 
                           std::to_wstring(seg.sequence_number));
            }
        }

        // Wait before next playlist refresh
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    AddDebugLog(L"DatapathIPC::StartStreaming: Streaming loop ended for " + config_.channel_name);
    SignalEndOfStream();
    
    return true;
}

void DatapathIPC::StopStreaming() {
    if (!is_active_.load()) return;

    AddDebugLog(L"DatapathIPC::StopStreaming: Stopping stream for " + config_.channel_name);

    should_stop_.store(true);
    buffer_condition_.notify_all();

    // Wait for threads to finish
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    if (buffer_manager_thread_.joinable()) {
        buffer_manager_thread_.join();
    }
    if (media_player_thread_.joinable()) {
        media_player_thread_.join();
    }
    if (named_pipe_thread_.joinable()) {
        named_pipe_thread_.join();
    }

    // Terminate media player if still running
    if (player_started_ && player_process_info_.hProcess) {
        if (WaitForSingleObject(player_process_info_.hProcess, 1000) == WAIT_TIMEOUT) {
            TerminateProcess(player_process_info_.hProcess, 0);
        }
        CloseHandle(player_process_info_.hProcess);
        CloseHandle(player_process_info_.hThread);
        memset(&player_process_info_, 0, sizeof(player_process_info_));
        player_started_ = false;
    }

    CleanupResources();
    is_active_.store(false);

    AddDebugLog(L"DatapathIPC::StopStreaming: Stopped for " + config_.channel_name);
}

bool DatapathIPC::WriteSegmentData(const std::vector<char>& data, std::atomic<bool>& cancel_token) {
    if (!is_active_.load() || data.empty()) return false;

    bool any_success = false;

    // Write to Datapath clients if any are connected
    if (!connected_clients_.empty()) {
        bool datapath_success = WriteToDatapathClients(data);
        if (datapath_success) {
            any_success = true;
            AddDebugLog(L"DatapathIPC::WriteSegmentData: Successfully wrote to Datapath clients");
        }
    }
    
    // Write to named pipe if available
    if (config_.use_named_pipe_bridge && named_pipe_handle_ != INVALID_HANDLE_VALUE && named_pipe_active_.load()) {
        bool pipe_success = WriteToNamedPipe(data);
        if (pipe_success) {
            any_success = true;
            AddDebugLog(L"DatapathIPC::WriteSegmentData: Successfully wrote to stdin pipe");
        }
    }

    if (!any_success) {
        AddDebugLog(L"DatapathIPC::WriteSegmentData: No successful writes - no clients or pipe available");
    }

    return any_success;
}

bool DatapathIPC::IsActive() const {
    return is_active_.load();
}

size_t DatapathIPC::GetBufferSize() const {
    return buffer_size_.load();
}

std::wstring DatapathIPC::GetStatusInfo() const {
    std::wstringstream ss;
    ss << L"DatapathIPC[" << config_.channel_name << L"]: ";
    ss << L"Active=" << (is_active_.load() ? L"true" : L"false");
    ss << L", Buffer=" << buffer_size_.load() << L"/" << config_.max_buffer_segments;
    ss << L", Clients=" << connected_clients_.size();
    ss << L", EndOfStream=" << (end_of_stream_.load() ? L"true" : L"false");
    return ss.str();
}

void DatapathIPC::SignalEndOfStream() {
    end_of_stream_.store(true);
    buffer_condition_.notify_all();
}

// Private implementation methods

bool DatapathIPC::CreateDatapathServer() {
    AddDebugLog(L"[DATAPATH] CreateDatapathServer: Starting...");
    
    try {
        AddDebugLog(L"[DATAPATH] Converting datapath name to string...");
        std::string datapath_name_str(config_.datapath_name.begin(), config_.datapath_name.end());
        AddDebugLog(L"[DATAPATH] Datapath name (string): " + std::wstring(datapath_name_str.begin(), datapath_name_str.end()));
        
        AddDebugLog(L"[DATAPATH] Calling datapath::host...");
        datapath::error result = datapath::host(
            datapath_server_,
            datapath_name_str,
            datapath::permissions::User | datapath::permissions::Group | datapath::permissions::World,
            10 // max clients
        );

        AddDebugLog(L"[DATAPATH] datapath::host returned error code: " + std::to_wstring(static_cast<int>(result)));

        if (result != datapath::error::Success) {
            AddDebugLog(L"[DATAPATH] ERROR: CreateDatapathServer: Failed to create server, error=" + 
                       std::to_wstring(static_cast<int>(result)));
            return false;
        }

        AddDebugLog(L"[DATAPATH] Setting up event handlers...");
        // Set up event handlers
        datapath_server_->on_accept.add([this](bool& accept, std::shared_ptr<datapath::isocket> client) {
            OnClientConnect(accept, client);
        });

        AddDebugLog(L"[DATAPATH] CreateDatapathServer: Created Datapath server: " + config_.datapath_name);
        return true;
    }
    catch (const std::exception& e) {
        std::string error_msg(e.what());
        AddDebugLog(L"[DATAPATH] EXCEPTION in CreateDatapathServer: " + std::wstring(error_msg.begin(), error_msg.end()));
        return false;
    }
    catch (...) {
        AddDebugLog(L"[DATAPATH] UNKNOWN EXCEPTION in CreateDatapathServer");
        return false;
    }
}

bool DatapathIPC::CreateNamedPipeBridge() {
    // Create anonymous pipe for stdin communication with media player (like transport stream mode)
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
    
    // Create pipe with larger buffer for streaming
    DWORD pipe_buffer_size = 65536; // 64KB buffer
    
    if (!CreatePipe(&stdin_read_handle_, &named_pipe_handle_, &saAttr, pipe_buffer_size)) {
        AddDebugLog(L"DatapathIPC::CreateNamedPipeBridge: Failed to create stdin pipe, Error=" + 
                   std::to_wstring(GetLastError()));
        return false;
    }
    
    // Ensure the write handle is not inherited
    if (!SetHandleInformation(named_pipe_handle_, HANDLE_FLAG_INHERIT, 0)) {
        AddDebugLog(L"DatapathIPC::CreateNamedPipeBridge: Failed to set handle information, Error=" + 
                   std::to_wstring(GetLastError()));
        CloseHandle(stdin_read_handle_);
        CloseHandle(named_pipe_handle_);
        stdin_read_handle_ = INVALID_HANDLE_VALUE;
        named_pipe_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    named_pipe_active_.store(true);
    AddDebugLog(L"DatapathIPC::CreateNamedPipeBridge: Created stdin pipe for media player");
    return true;
}

bool DatapathIPC::LaunchMediaPlayer() {
    // Build command line for media player to read from stdin (like transport stream mode)
    std::wstring cmd = L"\"" + config_.player_path + L"\" -";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read_handle_; // Connect our pipe to media player's stdin
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    AddDebugLog(L"DatapathIPC::LaunchMediaPlayer: Launching with stdin pipe: " + cmd);

    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()),
        nullptr, nullptr, TRUE, // bInheritHandles = TRUE for stdin redirection
        CREATE_NEW_CONSOLE,
        nullptr, nullptr, &si, &player_process_info_
    );

    if (!success) {
        AddDebugLog(L"DatapathIPC::LaunchMediaPlayer: Failed to create process, Error=" + 
                   std::to_wstring(GetLastError()));
        return false;
    }

    // Close the read handle in parent process since child now owns it
    CloseHandle(stdin_read_handle_);
    stdin_read_handle_ = INVALID_HANDLE_VALUE;

    player_started_ = true;
    AddDebugLog(L"DatapathIPC::LaunchMediaPlayer: Successfully launched player with stdin pipe, PID=" + 
               std::to_wstring(player_process_info_.dwProcessId));
    return true;
}

void DatapathIPC::ServerThreadProc() {
    AddDebugLog(L"DatapathIPC::ServerThreadProc: Starting server thread for " + config_.channel_name);

    while (!should_stop_.load()) {
        // Server management and client handling
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Cleanup disconnected clients
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            connected_clients_.erase(
                std::remove_if(connected_clients_.begin(), connected_clients_.end(),
                    [](const std::shared_ptr<datapath::isocket>& client) {
                        return !client || !client->good();
                    }),
                connected_clients_.end()
            );
        }
    }

    AddDebugLog(L"DatapathIPC::ServerThreadProc: Server thread ending for " + config_.channel_name);
}

void DatapathIPC::NamedPipeThreadProc() {
    AddDebugLog(L"DatapathIPC::NamedPipeThreadProc: Starting stdin pipe monitor for " + config_.channel_name);

    // No need to wait for connection with anonymous pipes - they're immediately ready
    AddDebugLog(L"DatapathIPC::NamedPipeThreadProc: Stdin pipe ready for streaming");
    
    // Keep the connection monitor active while streaming
    while (!should_stop_.load() && named_pipe_active_.load()) {
        // Check if media player process is still running
        if (player_started_ && player_process_info_.hProcess != INVALID_HANDLE_VALUE) {
            DWORD wait_result = WaitForSingleObject(player_process_info_.hProcess, 0);
            if (wait_result == WAIT_OBJECT_0) {
                AddDebugLog(L"DatapathIPC::NamedPipeThreadProc: Media player process ended");
                named_pipe_active_.store(false);
                break;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AddDebugLog(L"DatapathIPC::NamedPipeThreadProc: Stdin pipe monitor ending for " + config_.channel_name);
}

void DatapathIPC::BufferManagerThreadProc() {
    AddDebugLog(L"DatapathIPC::BufferManagerThreadProc: Starting buffer manager for " + config_.channel_name);

    while (!should_stop_.load()) {
        std::vector<char> segment_data;
        
        // Wait for data or stop signal
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_condition_.wait(lock, [this] {
                return !segment_buffer_.empty() || should_stop_.load() || end_of_stream_.load();
            });

            if (should_stop_.load()) break;

            if (!segment_buffer_.empty()) {
                segment_data = std::move(segment_buffer_.front());
                segment_buffer_.pop();
                buffer_size_.store(segment_buffer_.size());
            }
        }

        if (!segment_data.empty()) {
            // Send data to all connected clients
            std::atomic<bool> dummy_cancel(false);
            
            AddDebugLog(L"DatapathIPC::BufferManagerThreadProc: About to send segment (" + 
                       std::to_wstring(segment_data.size()) + L" bytes), buffer=" + 
                       std::to_wstring(buffer_size_.load()));
            
            bool write_success = WriteSegmentData(segment_data, dummy_cancel);
            
            AddDebugLog(L"DatapathIPC::BufferManagerThreadProc: Sent segment, success=" + 
                       std::to_wstring(write_success) + L", buffer=" + 
                       std::to_wstring(buffer_size_.load()));
                       
            // Add small delay between segments to prevent overwhelming the media player
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (end_of_stream_.load() && buffer_size_.load() == 0) {
            AddDebugLog(L"DatapathIPC::BufferManagerThreadProc: End of stream reached, stopping");
            break;
        }
    }

    AddDebugLog(L"DatapathIPC::BufferManagerThreadProc: Buffer manager ending for " + config_.channel_name);
}

void DatapathIPC::MediaPlayerThreadProc() {
    AddDebugLog(L"DatapathIPC::MediaPlayerThreadProc: Starting media player monitor for " + config_.channel_name);

    if (!player_started_ || !player_process_info_.hProcess) {
        AddDebugLog(L"DatapathIPC::MediaPlayerThreadProc: No player process to monitor");
        return;
    }

    // Monitor player process
    while (!should_stop_.load()) {
        DWORD wait_result = WaitForSingleObject(player_process_info_.hProcess, 1000);
        
        if (wait_result == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            GetExitCodeProcess(player_process_info_.hProcess, &exit_code);
            AddDebugLog(L"DatapathIPC::MediaPlayerThreadProc: Player process exited with code " + 
                       std::to_wstring(exit_code));
            should_stop_.store(true);
            break;
        }
    }

    AddDebugLog(L"DatapathIPC::MediaPlayerThreadProc: Media player monitor ending for " + config_.channel_name);
}

void DatapathIPC::CleanupResources() {
    // Close Datapath server
    if (datapath_server_) {
        datapath_server_->close();
        datapath_server_.reset();
    }

    // Clear connected clients
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& client : connected_clients_) {
            if (client) {
                client->close();
            }
        }
        connected_clients_.clear();
    }

    // Close named pipe handles
    if (named_pipe_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(named_pipe_handle_);
        named_pipe_handle_ = INVALID_HANDLE_VALUE;
    }
    if (stdin_read_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_read_handle_);
        stdin_read_handle_ = INVALID_HANDLE_VALUE;
    }
    named_pipe_active_.store(false);

    // Clear buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        while (!segment_buffer_.empty()) {
            segment_buffer_.pop();
        }
        buffer_size_.store(0);
    }
    
    // Clear sequence tracking
    {
        std::lock_guard<std::mutex> lock(sequence_mutex_);
        sequence_ordered_segments_.clear();
        seen_urls_.clear();
        next_expected_sequence_ = 0.0;
        last_processed_sequence_ = -1.0;
    }
}

// Event handlers

void DatapathIPC::OnClientConnect(bool& accept, std::shared_ptr<datapath::isocket> client) {
    if (!client || !client->good()) {
        accept = false;
        return;
    }

    AddDebugLog(L"DatapathIPC::OnClientConnect: New client connecting to " + config_.channel_name);

    // Set up client event handlers
    client->on_message.add([this, client](const std::vector<char>& data) {
        OnClientMessage(client, data);
    });

    client->on_close.add([this, client]() {
        OnClientDisconnect(client);
    });

    // Add to connected clients list
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        connected_clients_.push_back(client);
    }

    accept = true;
    AddDebugLog(L"DatapathIPC::OnClientConnect: Client accepted, total clients=" + 
               std::to_wstring(connected_clients_.size()));
}

void DatapathIPC::OnClientMessage(std::shared_ptr<datapath::isocket> client, const std::vector<char>& data) {
    // Handle messages from clients if needed (usually not needed for streaming)
    AddDebugLog(L"DatapathIPC::OnClientMessage: Received " + std::to_wstring(data.size()) + 
               L" bytes from client");
}

void DatapathIPC::OnClientDisconnect(std::shared_ptr<datapath::isocket> client) {
    AddDebugLog(L"DatapathIPC::OnClientDisconnect: Client disconnected from " + config_.channel_name);

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        connected_clients_.erase(
            std::remove(connected_clients_.begin(), connected_clients_.end(), client),
            connected_clients_.end()
        );
    }

    AddDebugLog(L"DatapathIPC::OnClientDisconnect: Remaining clients=" + 
               std::to_wstring(connected_clients_.size()));
}

// Utility methods

std::wstring DatapathIPC::GenerateDatapathName(const std::wstring& channel) const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::wstringstream ss;
    ss << L"TardsplayaDatapath_" << channel << L"_" << time_t;
    return ss.str();
}

std::wstring DatapathIPC::GenerateNamedPipeName(const std::wstring& channel) const {
    std::wstringstream ss;
    ss << L"\\\\.\\pipe\\TardsplayaStream_" << channel;
    return ss.str();
}

bool DatapathIPC::WriteToDatapathClients(const std::vector<char>& data) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    bool any_success = false;
    for (auto& client : connected_clients_) {
        if (!client || !client->good()) continue;

        try {
            std::shared_ptr<datapath::itask> task;
            datapath::error result = client->write(task, data);
            
            if (result == datapath::error::Success) {
                any_success = true;
            } else {
                AddDebugLog(L"DatapathIPC::WriteToDatapathClients: Write failed, error=" + 
                           std::to_wstring(static_cast<int>(result)));
            }
        }
        catch (const std::exception&) {
            AddDebugLog(L"DatapathIPC::WriteToDatapathClients: Exception writing to client");
        }
    }

    return any_success;
}

bool DatapathIPC::WriteToNamedPipe(const std::vector<char>& data) {
    if (named_pipe_handle_ == INVALID_HANDLE_VALUE || !named_pipe_active_.load()) {
        AddDebugLog(L"DatapathIPC::WriteToNamedPipe: Stdin pipe not available (handle=" + 
                   std::to_wstring(reinterpret_cast<uintptr_t>(named_pipe_handle_)) + 
                   L", active=" + (named_pipe_active_.load() ? L"true" : L"false") + L")");
        return false;
    }

    // Use retry logic similar to legacy pipe writing
    const int max_write_attempts = 3;
    for (int write_attempt = 1; write_attempt <= max_write_attempts; ++write_attempt) {
        DWORD bytes_written = 0;
        BOOL result = WriteFile(
            named_pipe_handle_,
            data.data(),
            static_cast<DWORD>(data.size()),
            &bytes_written,
            nullptr
        );

        if (result && bytes_written == data.size()) {
            AddDebugLog(L"DatapathIPC::WriteToNamedPipe: Successfully wrote " + 
                       std::to_wstring(bytes_written) + L" bytes to stdin pipe");
            return true;
        }

        DWORD error = GetLastError();
        
        // Only treat ERROR_BROKEN_PIPE as immediate disconnection
        if (error == ERROR_BROKEN_PIPE) {
            AddDebugLog(L"DatapathIPC::WriteToNamedPipe: Stdin pipe broken (error=" + 
                       std::to_wstring(error) + L")");
            named_pipe_active_.store(false);
            return false;
        }
        
        // For other errors (including ERROR_NO_DATA), retry with delay
        if (write_attempt < max_write_attempts) {
            AddDebugLog(L"DatapathIPC::WriteToNamedPipe: Write attempt " + 
                       std::to_wstring(write_attempt) + L"/" + std::to_wstring(max_write_attempts) + 
                       L" failed (error=" + std::to_wstring(error) + L"), retrying...");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            AddDebugLog(L"DatapathIPC::WriteToNamedPipe: All write attempts failed, Error=" + 
                       std::to_wstring(error) + L", BytesWritten=" + std::to_wstring(bytes_written) + 
                       L"/" + std::to_wstring(data.size()));
            return false;
        }
    }

    return false;
}

void DatapathIPC::ProcessSequencedSegments() {
    std::lock_guard<std::mutex> seq_lock(sequence_mutex_);
    
    // Move segments that are ready (in sequence order) to the playback buffer
    while (!sequence_ordered_segments_.empty()) {
        auto it = sequence_ordered_segments_.find(next_expected_sequence_);
        if (it == sequence_ordered_segments_.end()) {
            // Next expected segment is not available yet
            break;
        }
        
        // Move this segment to the playback buffer
        std::vector<char> segment_data = std::move(it->second);
        sequence_ordered_segments_.erase(it);
        
        {
            std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
            segment_buffer_.push(std::move(segment_data));
            buffer_size_.store(segment_buffer_.size());
        }
        buffer_condition_.notify_one();
        
        AddDebugLog(L"DatapathIPC::ProcessSequencedSegments: Released segment sequence=" + 
                   std::to_wstring(next_expected_sequence_) + L" to playback buffer");
        
        last_processed_sequence_ = next_expected_sequence_;
        next_expected_sequence_ += 1.0;
    }
    
    // Clean up very old segments that might be stuck (older than last processed - 10)
    auto cleanup_threshold = last_processed_sequence_ - 10.0;
    auto cleanup_it = sequence_ordered_segments_.begin();
    while (cleanup_it != sequence_ordered_segments_.end()) {
        if (cleanup_it->first < cleanup_threshold) {
            AddDebugLog(L"DatapathIPC::ProcessSequencedSegments: Cleaning up old segment sequence=" + 
                       std::to_wstring(cleanup_it->first));
            cleanup_it = sequence_ordered_segments_.erase(cleanup_it);
        } else {
            ++cleanup_it;
        }
    }
}

// Compatibility function

bool BufferAndPipeStreamToPlayerDatapath(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality,
    HANDLE* player_process_handle
) {
    AddDebugLog(L"***TEST*** DATAPATH FUNCTION ENTRY POINT REACHED");
    AddDebugLog(L"[DATAPATH] BufferAndPipeStreamToPlayerDatapath: ENTRY - Starting Datapath IPC streaming for " + channel_name);
    AddDebugLog(L"[DATAPATH] Parameters: player_path=" + player_path + L", buffer_segments=" + std::to_wstring(buffer_segments));
    AddDebugLog(L"***TEST*** DATAPATH FUNCTION PARAMETERS SET");

    try {
        AddDebugLog(L"***TEST*** ENTERING TRY BLOCK IN DATAPATH FUNCTION");
        // Create and configure Datapath IPC
        AddDebugLog(L"[DATAPATH] Creating DatapathIPC instance...");
        AddDebugLog(L"***TEST*** ABOUT TO CREATE DATAPATH IPC INSTANCE");
        DatapathIPC datapath_ipc;
        AddDebugLog(L"***TEST*** DATAPATH IPC INSTANCE CREATED SUCCESSFULLY");
        
        AddDebugLog(L"[DATAPATH] Configuring DatapathIPC...");
        DatapathIPC::Config config;
        config.channel_name = channel_name;
        config.player_path = player_path;
        config.max_buffer_segments = std::max(buffer_segments, 3);
        config.use_named_pipe_bridge = true;

        AddDebugLog(L"[DATAPATH] Calling Initialize...");
        if (!datapath_ipc.Initialize(config)) {
            AddDebugLog(L"[DATAPATH] ERROR: Failed to initialize Datapath IPC - falling back to legacy");
            return false;
        }

        AddDebugLog(L"[DATAPATH] Initialize succeeded, calling StartStreaming...");
        // Start streaming
        bool result = datapath_ipc.StartStreaming(playlist_url, cancel_token, chunk_count, player_process_handle);
        
        if (result) {
            AddDebugLog(L"[DATAPATH] SUCCESS: Streaming completed successfully for " + channel_name);
        } else {
            AddDebugLog(L"[DATAPATH] ERROR: Streaming failed for " + channel_name);
        }

        return result;
    } catch (const std::exception& e) {
        std::string error_msg(e.what());
        AddDebugLog(L"[DATAPATH] EXCEPTION: " + std::wstring(error_msg.begin(), error_msg.end()));
        return false;
    } catch (...) {
        AddDebugLog(L"[DATAPATH] UNKNOWN EXCEPTION occurred");
        return false;
    }
}