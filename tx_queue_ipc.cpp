#include "tx_queue_ipc.h"
#include "stream_thread.h"
#include "stream_pipe.h"
#include "enhanced_playlist_parser.h"
#include "tsduck_hls_wrapper.h"
#include <sstream>
#include <iomanip>
#include <regex>
#include <chrono>
#include <winhttp.h>
#include <random>
#include <set>

// Include existing utility functions
extern std::string WideToUtf8(const std::wstring& w);
extern std::wstring Utf8ToWide(const std::string& s);
extern bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token);

// Helper function to join URLs
static std::wstring JoinUrl(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}

#pragma comment(lib, "winhttp.lib")

using namespace qcstudio;
using namespace tardsplaya;

namespace {
    // Helper function for HTTP binary downloads
    bool HttpGetBinary(const std::wstring& url, std::vector<char>& out, std::atomic<bool>* cancel_token = nullptr) {
        // Reuse existing HTTP implementation from stream_pipe.cpp
        std::string text_data;
        if (!HttpGetText(url, text_data, cancel_token)) {
            return false;
        }
        out.assign(text_data.begin(), text_data.end());
        return true;
    }
}

// TxQueueIPC Implementation
TxQueueIPC::TxQueueIPC(uint64_t queue_capacity) : queue_capacity_(queue_capacity) {
    AddDebugLog(L"[TX-QUEUE] Creating IPC manager with capacity: " + std::to_wstring(queue_capacity_) + L" bytes");
}

TxQueueIPC::~TxQueueIPC() {
    AddDebugLog(L"[TX-QUEUE] Destroying IPC manager");
}

bool TxQueueIPC::Initialize() {
    try {
        // Create single-process tx-queue with proper alignment
        // Use aligned allocation to avoid C4316 warning
#ifdef _WIN32
        void* aligned_ptr = _aligned_malloc(sizeof(qcstudio::tx_queue_sp_t), 64);
        if (!aligned_ptr) {
            AddDebugLog(L"[TX-QUEUE] Failed to allocate aligned memory for tx-queue");
            return false;
        }
        queue_.reset(new(aligned_ptr) qcstudio::tx_queue_sp_t(queue_capacity_));
#else
        void* aligned_ptr = aligned_alloc(64, sizeof(qcstudio::tx_queue_sp_t));
        if (!aligned_ptr) {
            AddDebugLog(L"[TX-QUEUE] Failed to allocate aligned memory for tx-queue");
            return false;
        }
        queue_.reset(new(aligned_ptr) qcstudio::tx_queue_sp_t(queue_capacity_));
#endif
        
        if (!queue_ || !queue_->is_ok()) {
            AddDebugLog(L"[TX-QUEUE] Failed to create tx-queue");
            return false;
        }
        
        initialized_ = true;
        AddDebugLog(L"[TX-QUEUE] Initialized successfully with capacity: " + 
                   std::to_wstring(queue_->capacity()) + L" bytes");
        return true;
        
    } catch (const std::exception& e) {
        AddDebugLog(L"[TX-QUEUE] Exception during initialization: " + Utf8ToWide(e.what()));
        return false;
    }
}

bool TxQueueIPC::ProduceSegment(std::vector<char>&& segment_data, bool has_discontinuity) {
    if (!IsReady()) {
        AddDebugLog(L"[TX-QUEUE] Cannot produce - IPC not ready");
        return false;
    }
    
    // Create segment with sequence number and discontinuity flag
    StreamSegment segment(std::move(segment_data), sequence_counter_++, has_discontinuity);
    
    bool success = WriteSegmentToQueue(segment);
    if (success) {
        produced_count_++;
        std::wstring disc_info = segment.has_discontinuity ? L" [DISCONTINUITY]" : L"";
        AddDebugLog(L"[TX-QUEUE] Produced segment #" + std::to_wstring(segment.sequence_number) + 
                   L", size: " + std::to_wstring(segment.data.size()) + L" bytes" + disc_info);
    } else {
        dropped_count_++;
        AddDebugLog(L"[TX-QUEUE] Dropped segment #" + std::to_wstring(segment.sequence_number) + 
                   L" - queue full or error");
    }
    
    return success;
}

bool TxQueueIPC::ConsumeSegment(StreamSegment& segment) {
    if (!IsReady()) {
        AddDebugLog(L"[TX-QUEUE] Cannot consume - IPC not ready");
        return false;
    }
    
    bool success = ReadSegmentFromQueue(segment);
    if (success) {
        consumed_count_++;
        if (!segment.verify_checksum()) {
            AddDebugLog(L"[TX-QUEUE] WARNING: Checksum mismatch for segment #" + 
                       std::to_wstring(segment.sequence_number));
        }
        AddDebugLog(L"[DEBUG] [TX-QUEUE] Consumed segment #" + std::to_wstring(segment.sequence_number) + 
                   L", size: " + std::to_wstring(segment.data.size()) + L" bytes");
    }
    // Only log failures occasionally to avoid spam
    else {
        static uint64_t failure_count = 0;
        if (++failure_count % 100 == 1) { // Log every 100th failure
            AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read segment from queue (count: " + 
                       std::to_wstring(failure_count) + L")");
        }
    }
    
    return success;
}

void TxQueueIPC::SignalEndOfStream() {
    end_of_stream_ = true;
    
    // Add end marker to queue
    StreamSegment end_marker;
    end_marker.is_end_marker = true;
    end_marker.sequence_number = sequence_counter_++;
    
    WriteSegmentToQueue(end_marker);
    AddDebugLog(L"[TX-QUEUE] End of stream signaled");
}

bool TxQueueIPC::IsQueueNearFull() const {
    if (!queue_) return false;
    
    uint64_t capacity = queue_->capacity();
    uint64_t used = produced_count_.load() - consumed_count_.load();
    
    return (used * 100 / capacity) > 90; // >90% full
}

bool TxQueueIPC::WriteSegmentToQueue(const StreamSegment& segment) {
    if (!queue_) return false;
    
    try {
        if (auto write_op = tx_write_t<qcstudio::tx_queue_sp_t>(*queue_)) {
            // Write segment header
            write_op.write(segment.sequence_number);
            write_op.write(segment.checksum);
            write_op.write(segment.is_end_marker);
            write_op.write(segment.has_discontinuity);
            write_op.write(static_cast<uint32_t>(segment.data.size()));
            
            // Write segment data
            if (!segment.data.empty()) {
                write_op.write(segment.data.data(), segment.data.size());
            }
            
            return true; // write_op commits on destruction
        }
    } catch (const std::exception& e) {
        AddDebugLog(L"[TX-QUEUE] Write exception: " + Utf8ToWide(e.what()));
    }
    
    return false;
}

bool TxQueueIPC::ReadSegmentFromQueue(StreamSegment& segment) {
    if (!queue_) return false;
    
    try {
        
        if (auto read_op = tx_read_t<qcstudio::tx_queue_sp_t>(*queue_)) {
            // Read segment header
            uint32_t data_size;
            if (!read_op.read(segment.sequence_number)) {
                // Reduce log spam - only log occasionally as these are expected failures
                static uint64_t seq_failure_count = 0;
                if (++seq_failure_count % 1000 == 1) { // Log every 1000th failure
                    AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read sequence number (count: " + 
                               std::to_wstring(seq_failure_count) + L")");
                }
                return false;
            }
            if (!read_op.read(segment.checksum)) {
                // Reduce log spam for checksum read failures too
                static uint64_t checksum_failure_count = 0;
                if (++checksum_failure_count % 1000 == 1) {
                    AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read checksum (count: " + 
                               std::to_wstring(checksum_failure_count) + L")");
                }
                return false;
            }
            if (!read_op.read(segment.is_end_marker)) {
                // Reduce log spam for end marker read failures too
                static uint64_t marker_failure_count = 0;
                if (++marker_failure_count % 1000 == 1) {
                    AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read end marker (count: " + 
                               std::to_wstring(marker_failure_count) + L")");
                }
                return false;
            }
            if (!read_op.read(segment.has_discontinuity)) {
                // Reduce log spam for discontinuity read failures too
                static uint64_t disc_failure_count = 0;
                if (++disc_failure_count % 1000 == 1) {
                    AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read discontinuity flag (count: " + 
                               std::to_wstring(disc_failure_count) + L")");
                }
                return false;
            }
            if (!read_op.read(data_size)) {
                // Reduce log spam for data size read failures too
                static uint64_t size_failure_count = 0;
                if (++size_failure_count % 1000 == 1) {
                    AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read data size (count: " + 
                               std::to_wstring(size_failure_count) + L")");
                }
                return false;
            }
            
            // Validate data size is reasonable (prevent buffer overflow)
            if (data_size > 16 * 1024 * 1024) { // 16MB max segment size
                AddDebugLog(L"[TX-QUEUE] Invalid data size: " + std::to_wstring(data_size) + L" bytes");
                return false;
            }
            
            // Read segment data
            if (data_size > 0) {
                segment.data.resize(data_size);
                if (!read_op.read(segment.data.data(), data_size)) {
                    // Reduce log spam for data read failures too
                    static uint64_t data_failure_count = 0;
                    if (++data_failure_count % 1000 == 1) {
                        AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to read segment data (" + std::to_wstring(data_size) + 
                                   L" bytes, count: " + std::to_wstring(data_failure_count) + L")");
                    }
                    return false;
                }
            } else {
                segment.data.clear();
            }
            
            return true; // read_op commits on destruction
        } else {
            // Reduce log spam for transaction creation failures - these are very common
            static uint64_t transaction_failure_count = 0;
            if (++transaction_failure_count % 2000 == 1) { // Log every 2000th failure
                AddDebugLog(L"[DEBUG] [TX-QUEUE] Failed to create read transaction (count: " + 
                           std::to_wstring(transaction_failure_count) + L")");
            }
            return false;
        }
    } catch (const std::exception& e) {
        AddDebugLog(L"[TX-QUEUE] Read exception: " + Utf8ToWide(e.what()));
    }
    
    return false;
}

// NamedPipeManager Implementation
NamedPipeManager::NamedPipeManager(const std::wstring& player_path) 
    : player_path_(player_path), pipe_handle_(INVALID_HANDLE_VALUE), 
      player_process_(INVALID_HANDLE_VALUE), initialized_(false), use_named_pipe_(false) {
    ZeroMemory(&process_info_, sizeof(process_info_));
}

NamedPipeManager::~NamedPipeManager() {
    Cleanup();
}

bool NamedPipeManager::Initialize(const std::wstring& channel_name) {
    AddDebugLog(L"[PIPE] Initializing pipe manager for channel: " + channel_name);
    
    // Launch player process (this handles both named pipes and stdin)
    if (!CreatePlayerProcess(channel_name)) {
        AddDebugLog(L"[PIPE] Failed to create player process");
        return false;
    }
    
    initialized_ = true;
    if (use_named_pipe_) {
        AddDebugLog(L"[PIPE] Initialized successfully with named pipe: " + pipe_name_);
    } else {
        AddDebugLog(L"[PIPE] Initialized successfully with stdin pipe");
    }
    return true;
}

bool NamedPipeManager::WriteToPlayer(const char* data, size_t size) {
    if (!initialized_ || pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD bytes_written = 0;
    BOOL result = WriteFile(pipe_handle_, data, static_cast<DWORD>(size), &bytes_written, nullptr);
    
    if (!result || bytes_written != size) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
            AddDebugLog(L"[PIPE] Player disconnected (broken pipe)");
        } else {
            AddDebugLog(L"[PIPE] Write failed, error: " + std::to_wstring(error));
        }
        return false;
    }
    
    return true;
}

bool NamedPipeManager::IsPlayerRunning() const {
    if (player_process_ == INVALID_HANDLE_VALUE) return false;
    
    DWORD exit_code;
    if (!GetExitCodeProcess(player_process_, &exit_code)) {
        return false;
    }
    
    return exit_code == STILL_ACTIVE;
}

void NamedPipeManager::Cleanup() {
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
    
    if (player_process_ != INVALID_HANDLE_VALUE) {
        if (IsPlayerRunning()) {
            TerminateProcess(player_process_, 0);
            WaitForSingleObject(player_process_, 2000);
        }
        CloseHandle(player_process_);
        player_process_ = INVALID_HANDLE_VALUE;
    }
    
    if (process_info_.hThread != nullptr) {
        CloseHandle(process_info_.hThread);
    }
    
    initialized_ = false;
}

std::wstring NamedPipeManager::GenerateUniquePipeName() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    return L"\\\\.\\pipe\\tardsplaya_" + std::to_wstring(GetCurrentProcessId()) + 
           L"_" + std::to_wstring(dis(gen));
}

bool NamedPipeManager::CreatePlayerProcess(const std::wstring& channel_name) {
    // Build command line for player
    std::wstring cmd_line;
    if (player_path_.find(L"mpv") != std::wstring::npos) {
        // For MPV, use stdin instead of named pipe
        cmd_line = L"\"" + player_path_ + L"\" --title=\"" + channel_name + 
                   L"\" --cache=yes --cache-secs=10 -";
    } else if (player_path_.find(L"vlc") != std::wstring::npos) {
        // For VLC, use stdin instead of named pipe
        cmd_line = L"\"" + player_path_ + L"\" --meta-title=\"" + channel_name + 
                   L"\" --file-caching=5000 -";
    } else if (player_path_.find(L"mpc") != std::wstring::npos || 
               player_path_.find(L"MPC") != std::wstring::npos) {
        // For MPC-HC/BE, use standard stdin streaming
        cmd_line = L"\"" + player_path_ + L"\" -";
        use_named_pipe_ = false;
    } else {
        // Generic player - use stdin
        cmd_line = L"\"" + player_path_ + L"\" -";
    }
    
    STARTUPINFOW si = { sizeof(si) };
    ZeroMemory(&process_info_, sizeof(process_info_));
    
    if (use_named_pipe_) {
        // This shouldn't be reached now, but keep for safety
        AddDebugLog(L"[PIPE] Warning: Named pipe mode not implemented for current player");
        return false;
    } else {
        // For other players: Create stdin pipe
        HANDLE hStdinRead, hStdinWrite;
        SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        
        // Use standard pipe buffer size for all players
        DWORD pipe_buffer_size = 1024 * 1024; // 1MB for all players
        
        if (!CreatePipe(&hStdinRead, &hStdinWrite, &saAttr, pipe_buffer_size)) {
            AddDebugLog(L"[PIPE] Failed to create stdin pipe");
            return false;
        }
        
        // Ensure write handle is not inherited
        if (!SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
            AddDebugLog(L"[PIPE] Failed to set handle information");
            CloseHandle(hStdinRead);
            CloseHandle(hStdinWrite);
            return false;
        }
        
        si.hStdInput = hStdinRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.dwFlags |= STARTF_USESTDHANDLES;
        
        BOOL result = CreateProcessW(
            nullptr,
            const_cast<LPWSTR>(cmd_line.c_str()),
            nullptr, nullptr, TRUE, // Inherit handles for stdin
            CREATE_NEW_CONSOLE,
            nullptr, nullptr,
            &si, &process_info_
        );
        
        // Close read handle since child process owns it now
        CloseHandle(hStdinRead);
        
        if (!result) {
            DWORD error = GetLastError();
            AddDebugLog(L"[PIPE] Failed to create player process, error: " + std::to_wstring(error));
            CloseHandle(hStdinWrite);
            return false;
        }
        
        // Store the write handle as our "pipe"
        pipe_handle_ = hStdinWrite;
    }
    player_process_ = process_info_.hProcess;
    
    AddDebugLog(L"[PIPE] Player process created, PID: " + std::to_wstring(process_info_.dwProcessId));
    return true;
}

bool NamedPipeManager::CreatePipeWithPlayer() {
    // For stdin piping, we don't create a named pipe here
    // The pipe is created in CreatePlayerProcess
    AddDebugLog(L"[PIPE] Using stdin piping instead of named pipe");
    return true;
}

// TxQueueStreamManager Implementation
TxQueueStreamManager::TxQueueStreamManager(const std::wstring& player_path, const std::wstring& channel_name)
    : player_path_(player_path), channel_name_(channel_name) {
    AddDebugLog(L"[STREAM] Creating TX-Queue stream manager for: " + channel_name);
}

TxQueueStreamManager::~TxQueueStreamManager() {
    StopStreaming();
    AddDebugLog(L"[STREAM] TX-Queue stream manager destroyed");
}

bool TxQueueStreamManager::Initialize() {
    // Create IPC manager
    ipc_manager_ = std::make_unique<TxQueueIPC>();
    if (!ipc_manager_->Initialize()) {
        AddDebugLog(L"[STREAM] Failed to initialize TX-Queue IPC");
        return false;
    }
    
    // Create pipe manager
    pipe_manager_ = std::make_unique<NamedPipeManager>(player_path_);
    if (!pipe_manager_->Initialize(channel_name_)) {
        AddDebugLog(L"[STREAM] Failed to initialize named pipe manager");
        return false;
    }
    
    AddDebugLog(L"[STREAM] Stream manager initialized successfully");
    return true;
}

bool TxQueueStreamManager::StartStreaming(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    std::atomic<int>* chunk_count) {
    
    if (streaming_active_.load()) {
        AddDebugLog(L"[STREAM] Streaming already active");
        return false;
    }
    
    cancel_token_ptr_ = &cancel_token;
    log_callback_ = log_callback;
    chunk_count_ptr_ = chunk_count;
    should_stop_ = false;
    
    // Start producer thread (downloads segments and feeds to tx-queue)
    producer_thread_ = std::thread(&TxQueueStreamManager::ProducerThreadFunction, this, playlist_url);
    
    // Start consumer thread (reads from tx-queue and feeds to player)
    consumer_thread_ = std::thread(&TxQueueStreamManager::ConsumerThreadFunction, this);
    
    streaming_active_ = true;
    AddDebugLog(L"[STREAM] Streaming started for: " + playlist_url);
    return true;
}

void TxQueueStreamManager::StopStreaming() {
    if (!streaming_active_.load()) return;
    
    should_stop_ = true;
    
    // Wait for threads to finish
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }
    
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
    
    streaming_active_ = false;
    AddDebugLog(L"[STREAM] Streaming stopped");
}

TxQueueStreamManager::StreamStats TxQueueStreamManager::GetStats() const {
    StreamStats stats = {};
    
    if (ipc_manager_) {
        stats.segments_produced = ipc_manager_->GetProducedCount();
        stats.segments_consumed = ipc_manager_->GetConsumedCount();
        stats.segments_dropped = ipc_manager_->GetDroppedCount();
        stats.queue_ready = ipc_manager_->IsReady();
    }
    
    if (pipe_manager_) {
        stats.player_running = pipe_manager_->IsPlayerRunning();
    }
    
    stats.bytes_transferred = bytes_transferred_.load();
    
    return stats;
}

void TxQueueStreamManager::ProducerThreadFunction(const std::wstring& playlist_url) {
    AddDebugLog(L"[PRODUCER] Starting producer thread for: " + playlist_url);
    
    std::set<std::wstring> seen_urls;
    int consecutive_errors = 0;
    const int max_errors = 10;
    tsduck_hls::PlaylistParser playlist_parser;
    
    while (!should_stop_.load() && (!cancel_token_ptr_ || !cancel_token_ptr_->load())) {
        // Download current playlist
        std::string playlist_content;
        if (!HttpGetText(playlist_url, playlist_content, cancel_token_ptr_)) {
            consecutive_errors++;
            LogMessage(L"[PRODUCER] Failed to download playlist, attempt " + 
                      std::to_wstring(consecutive_errors) + L"/" + std::to_wstring(max_errors));
            
            if (consecutive_errors >= max_errors) {
                LogMessage(L"[PRODUCER] Too many consecutive errors, stopping");
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        
        consecutive_errors = 0;
        
        // Apply discontinuity filtering to remove ad segments
        std::string original_playlist = playlist_content;
        try {
            playlist_content = FilterDiscontinuitySegments(playlist_content, "");
            
            // Count segments filtered
            std::istringstream original_ss(original_playlist);
            std::istringstream filtered_ss(playlist_content);
            std::string line;
            size_t original_segment_count = 0, filtered_segment_count = 0;
            
            while (std::getline(original_ss, line)) {
                if (!line.empty() && line[0] != '#') original_segment_count++;
            }
            while (std::getline(filtered_ss, line)) {
                if (!line.empty() && line[0] != '#') filtered_segment_count++;
            }
            
            size_t segments_removed = (original_segment_count > filtered_segment_count) ? 
                                    (original_segment_count - filtered_segment_count) : 0;
            
            if (segments_removed > 0) {
                LogMessage(L"[PRODUCER] Filtered out " + std::to_wstring(segments_removed) + 
                          L" discontinuity segments (ads) from playlist");
            }
        } catch (const std::exception& e) {
            LogMessage(L"[PRODUCER] Discontinuity filtering failed, using original playlist - Error: " + 
                      std::wstring(e.what(), e.what() + strlen(e.what())));
            playlist_content = original_playlist; // Fallback to original
        }
        
        // Parse playlist using TSDuck HLS wrapper for discontinuity detection
        if (!playlist_parser.ParsePlaylist(playlist_content)) {
            LogMessage(L"[PRODUCER] Failed to parse playlist with TSDuck wrapper");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        
        // Get segments with discontinuity information
        auto media_segments = playlist_parser.GetSegments();
        bool playlist_has_discontinuities = playlist_parser.HasDiscontinuities();
        
        if (playlist_has_discontinuities) {
            LogMessage(L"[PRODUCER] Discontinuities detected in playlist - buffer flushing enabled");
        }
        
        // Download new segments
        for (const auto& media_segment : media_segments) {
            if (should_stop_.load() || (cancel_token_ptr_ && cancel_token_ptr_->load())) break;
            
            // Convert segment URL to wide string and make absolute
            std::wstring segment_url = media_segment.url;
            if (segment_url.find(L"http") != 0) {
                segment_url = JoinUrl(playlist_url, segment_url);
            }
            
            // Skip already downloaded segments
            if (seen_urls.count(segment_url)) continue;
            seen_urls.insert(segment_url);
            
            // Check if queue is getting full
            if (ipc_manager_->IsQueueNearFull()) {
                LogMessage(L"[PRODUCER] Queue near full, pausing downloads");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            
            // Download segment
            std::vector<char> segment_data;
            if (DownloadSegment(segment_url, segment_data)) {
                // Add to tx-queue with discontinuity information
                bool has_discontinuity = media_segment.has_discontinuity;
                if (ipc_manager_->ProduceSegment(std::move(segment_data), has_discontinuity)) {
                    std::wstring disc_info = has_discontinuity ? L" [DISCONTINUITY]" : L"";
                    LogMessage(L"[PRODUCER] Queued segment from: " + 
                              segment_url.substr(segment_url.find_last_of(L'/') + 1) + disc_info);
                }
            }
        }
        
        // Update chunk count for UI
        if (chunk_count_ptr_) {
            auto stats = GetStats();
            uint64_t queue_depth = 0;
            if (stats.segments_produced >= stats.segments_consumed) {
                queue_depth = stats.segments_produced - stats.segments_consumed;
            }
            chunk_count_ptr_->store(static_cast<int>(queue_depth));
        }
        
        // Wait before next playlist fetch
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    
    // Signal end of stream
    ipc_manager_->SignalEndOfStream();
    AddDebugLog(L"[PRODUCER] Producer thread ending");
}

void TxQueueStreamManager::ConsumerThreadFunction() {
    AddDebugLog(L"[CONSUMER] Starting consumer thread");
    
    StreamSegment segment;
    bool initial_buffer_filled = false;
    
    // Standard buffer configuration for all players
    const int initial_buffer_size = 8; // Standard buffer size
    const size_t max_write_size = 2 * 1024 * 1024; // 2MB max write size
    
    while (!should_stop_.load() && (!cancel_token_ptr_ || !cancel_token_ptr_->load())) {
        // Check buffer status BEFORE consuming - this prevents race condition
        if (!initial_buffer_filled) {
            auto stats = GetStats();
            // Use safer arithmetic to avoid underflow
            uint64_t queue_depth = 0;
            if (stats.segments_produced >= stats.segments_consumed) {
                queue_depth = stats.segments_produced - stats.segments_consumed;
            }
            
            if (queue_depth >= static_cast<uint64_t>(initial_buffer_size)) {
                initial_buffer_filled = true;
                LogMessage(L"[CONSUMER] Initial buffer filled (" + 
                          std::to_wstring(queue_depth) + 
                          L" segments), starting playback");
            } else {
                LogMessage(L"[CONSUMER] Waiting for initial buffer to fill (" + 
                          std::to_wstring(queue_depth) + L"/" + 
                          std::to_wstring(initial_buffer_size) + L" segments)...");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        
        // Try to consume a segment from tx-queue
        if (!ipc_manager_->ConsumeSegment(segment)) {
            // No data available, check if we should continue waiting
            if (ipc_manager_->IsEndOfStream()) {
                LogMessage(L"[CONSUMER] End of stream reached");
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        
        // Check if this is end marker
        if (segment.is_end_marker) {
            LogMessage(L"[CONSUMER] End marker received");
            break;
        }
        
        // Handle discontinuities by logging them (no special processing)
        if (segment.has_discontinuity) {
            LogMessage(L"[CONSUMER] DISCONTINUITY detected in segment #" + std::to_wstring(segment.sequence_number) + 
                      L" - continuing normal processing");
        }
        
        if (initial_buffer_filled && !segment.data.empty()) {
            // Standard processing for all players: Write segments directly
            if (pipe_manager_->WriteToPlayer(segment.data.data(), segment.data.size())) {
                bytes_transferred_ += segment.data.size();
                std::wstring disc_info = segment.has_discontinuity ? L" [DISC]" : L"";
                LogMessage(L"[CONSUMER] Fed segment #" + std::to_wstring(segment.sequence_number) + 
                          L" to player (" + std::to_wstring(segment.data.size()) + L" bytes)" + disc_info);
            } else {
                LogMessage(L"[CONSUMER] Failed to write to player - may have disconnected");
                if (!pipe_manager_->IsPlayerRunning()) {
                    LogMessage(L"[CONSUMER] Player process died, stopping consumer");
                    break;
                }
            }
        }
        
        // Standard timing for all players
        if (segment.data.size() > 100 * 1024) { // >100KB = normal content
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else { // Small segment = likely ad content
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    AddDebugLog(L"[CONSUMER] Consumer thread ending");
}

bool TxQueueStreamManager::DownloadSegment(const std::wstring& segment_url, std::vector<char>& segment_data) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (HttpGetBinary(segment_url, segment_data, cancel_token_ptr_)) {
            return true;
        }
        
        if (should_stop_.load() || (cancel_token_ptr_ && cancel_token_ptr_->load())) {
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    
    return false;
}

void TxQueueStreamManager::LogMessage(const std::wstring& message) {
    if (log_callback_) {
        log_callback_(message);
    } else {
        AddDebugLog(message);
    }
}

void TxQueueStreamManager::UpdateChunkCount(int count) {
    if (chunk_count_ptr_) {
        chunk_count_ptr_->store(count);
    }
}