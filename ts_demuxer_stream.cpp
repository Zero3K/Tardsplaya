#include "ts_demuxer_stream.h"
#include "stream_thread.h"
#include "stream_pipe.h"
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

#pragma comment(lib, "winhttp.lib")

using namespace tardsplaya;

// Helper function to join URLs
static std::wstring JoinUrl(const std::wstring& base, const std::wstring& rel) {
    if (rel.find(L"http") == 0) return rel;
    size_t pos = base.rfind(L'/');
    if (pos == std::wstring::npos) return rel;
    return base.substr(0, pos + 1) + rel;
}

// Helper function for HTTP binary downloads
static bool HttpGetBinary(const std::wstring& url, std::vector<char>& out, std::atomic<bool>* cancel_token = nullptr) {
    std::string text_data;
    if (!HttpGetText(url, text_data, cancel_token)) {
        return false;
    }
    out.assign(text_data.begin(), text_data.end());
    return true;
}

// MemoryTSDemuxer Implementation
MemoryTSDemuxer::MemoryTSDemuxer() 
    : buffer_(nullptr), buffer_size_(0), current_offset_(0), packet_size_(188),
      pmt_pid_(0), pcr_pid_(0), video_pid_(0), audio_pid_(0) {
    memset(&stats_, 0, sizeof(stats_));
}

MemoryTSDemuxer::~MemoryTSDemuxer() {
}

bool MemoryTSDemuxer::Initialize(const char* buffer, size_t buffer_size) {
    buffer_ = buffer;
    buffer_size_ = buffer_size;
    current_offset_ = 0;
    
    // Reset state
    pmt_pid_ = 0;
    pcr_pid_ = 0;
    video_pid_ = 0;
    audio_pid_ = 0;
    memset(&stats_, 0, sizeof(stats_));
    
    return DetectPacketSize();
}

bool MemoryTSDemuxer::DetectPacketSize() {
    if (!buffer_ || buffer_size_ < 1024) return false;
    
    // Look for sync bytes to determine packet size
    for (size_t i = 0; i < std::min(buffer_size_, size_t(512)); i++) {
        if (buffer_[i] == 0x47) { // TS sync byte
            // Check for consistent sync bytes at 188, 192, or 204 byte intervals
            bool found_188 = false, found_192 = false, found_204 = false;
            
            if (i + 188 < buffer_size_ && buffer_[i + 188] == 0x47) found_188 = true;
            if (i + 192 < buffer_size_ && buffer_[i + 192] == 0x47) found_192 = true;
            if (i + 204 < buffer_size_ && buffer_[i + 204] == 0x47) found_204 = true;
            
            if (found_188) {
                packet_size_ = 188;
                current_offset_ = i;
                return true;
            }
            if (found_192) {
                packet_size_ = 192;
                current_offset_ = i;
                return true;
            }
            if (found_204) {
                packet_size_ = 204;
                current_offset_ = i;
                return true;
            }
        }
    }
    
    return false;
}

bool MemoryTSDemuxer::SetVideoOutput(std::function<bool(const unsigned char*, unsigned int)> video_handler) {
    video_handler_ = video_handler;
    return true;
}

bool MemoryTSDemuxer::SetAudioOutput(std::function<bool(const unsigned char*, unsigned int)> audio_handler) {
    audio_handler_ = audio_handler;
    return true;
}

bool MemoryTSDemuxer::Process() {
    if (!buffer_ || buffer_size_ == 0) return false;
    
    while (current_offset_ + packet_size_ <= buffer_size_) {
        const unsigned char* packet = reinterpret_cast<const unsigned char*>(buffer_ + current_offset_);
        
        if (packet[0] != 0x47) {
            // Lost sync, try to find it again
            bool found_sync = false;
            for (size_t i = current_offset_ + 1; i < buffer_size_; i++) {
                if (buffer_[i] == 0x47) {
                    current_offset_ = i;
                    found_sync = true;
                    break;
                }
            }
            if (!found_sync) break;
            continue;
        }
        
        if (!ParseTSPacket(packet)) {
            AddDebugLog(L"[TS_DEMUX] Failed to parse TS packet");
        }
        
        stats_.packets_processed++;
        current_offset_ += packet_size_;
    }
    
    stats_.video_pid = video_pid_;
    stats_.audio_pid = audio_pid_;
    stats_.pcr_pid = pcr_pid_;
    stats_.has_video = (video_pid_ != 0);
    stats_.has_audio = (audio_pid_ != 0);
    
    return true;
}

bool MemoryTSDemuxer::ParseTSPacket(const unsigned char* packet) {
    // Parse TS header
    bool unit_start = (packet[1] & 0x40) != 0;
    unsigned int pid = ((packet[1] & 0x1F) << 8) | packet[2];
    unsigned int field_ctrl = (packet[3] & 0x30) >> 4;
    
    const unsigned char* payload = nullptr;
    unsigned int payload_len = 0;
    
    switch (field_ctrl) {
        case 1: // Payload only
            payload = packet + 4;
            payload_len = packet_size_ - 4;
            break;
        case 3: // Adaptation field + payload
            if (packet_size_ > 5 && packet[4] < packet_size_ - 5) {
                payload = packet + 5 + packet[4];
                payload_len = packet_size_ - 5 - packet[4];
            }
            break;
        default:
            return true; // No payload
    }
    
    if (!payload || payload_len == 0) return true;
    
    // Handle different PID types
    if (pid == 0x0000) { // PAT
        if (unit_start) {
            return ParsePAT(payload, payload_len);
        }
    } else if (pmt_pid_ != 0 && pid == pmt_pid_) { // PMT
        if (unit_start) {
            return ParsePMT(payload, payload_len);
        }
    } else if (video_pid_ != 0 && pid == video_pid_) { // Video
        return ParsePES(payload, payload_len, pid, unit_start);
    } else if (audio_pid_ != 0 && pid == audio_pid_) { // Audio
        return ParsePES(payload, payload_len, pid, unit_start);
    }
    
    return true;
}

bool MemoryTSDemuxer::ParsePAT(const unsigned char* payload, unsigned int length) {
    if (length < 8) return false;
    
    unsigned int pointer = payload[0];
    if (pointer >= length) return false;
    
    const unsigned char* table = payload + 1 + pointer;
    unsigned int table_len = length - 1 - pointer;
    
    if (table_len < 8) return false;
    
    unsigned int section_length = ((table[1] & 0x03) << 8) | table[2];
    if (section_length < 4 || section_length + 3 > table_len) return false;
    
    // Skip to program list
    const unsigned char* programs = table + 8;
    unsigned int programs_len = section_length - 9; // Exclude CRC32
    
    for (unsigned int i = 0; i + 3 < programs_len; i += 4) {
        unsigned int program_num = (programs[i] << 8) | programs[i + 1];
        unsigned int program_pid = ((programs[i + 2] & 0x1F) << 8) | programs[i + 3];
        
        if (program_num != 0) { // Skip NIT PID
            pmt_pid_ = program_pid;
            break;
        }
    }
    
    return true;
}

bool MemoryTSDemuxer::ParsePMT(const unsigned char* payload, unsigned int length) {
    if (length < 8) return false;
    
    unsigned int pointer = payload[0];
    if (pointer >= length) return false;
    
    const unsigned char* table = payload + 1 + pointer;
    unsigned int table_len = length - 1 - pointer;
    
    if (table_len < 12) return false;
    
    unsigned int section_length = ((table[1] & 0x03) << 8) | table[2];
    if (section_length < 4 || section_length + 3 > table_len) return false;
    
    pcr_pid_ = ((table[8] & 0x1F) << 8) | table[9];
    unsigned int prog_info_len = ((table[10] & 0x03) << 8) | table[11];
    
    // Skip to elementary streams
    const unsigned char* streams = table + 12 + prog_info_len;
    unsigned int streams_len = section_length - 13 - prog_info_len; // Exclude CRC32
    
    for (unsigned int i = 0; i + 4 < streams_len;) {
        unsigned int stream_type = streams[i];
        unsigned int stream_pid = ((streams[i + 1] & 0x1F) << 8) | streams[i + 2];
        unsigned int es_info_len = ((streams[i + 3] & 0x03) << 8) | streams[i + 4];
        
        // Identify video and audio streams
        if (stream_type == 0x1B && video_pid_ == 0) { // H.264
            video_pid_ = stream_pid;
        } else if (stream_type == 0x0F && audio_pid_ == 0) { // ADTS AAC
            audio_pid_ = stream_pid;
        }
        
        i += 5 + es_info_len;
    }
    
    return true;
}

bool MemoryTSDemuxer::ParsePES(const unsigned char* payload, unsigned int length, unsigned int pid, bool unit_start) {
    if (!unit_start || length < 6) {
        // Continue with existing PES packet
        if (pid == video_pid_ && video_handler_) {
            return video_handler_(payload, length);
        } else if (pid == audio_pid_ && audio_handler_) {
            return audio_handler_(payload, length);
        }
        return true;
    }
    
    // Check for PES start code
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return true; // Not a PES packet
    }
    
    // Skip PES header to get to elementary stream data
    unsigned int pes_header_len = 6;
    if (length > 8) {
        pes_header_len += 3 + payload[8]; // Additional header fields
    }
    
    if (pes_header_len >= length) return true;
    
    const unsigned char* es_data = payload + pes_header_len;
    unsigned int es_len = length - pes_header_len;
    
    if (pid == video_pid_ && video_handler_) {
        return video_handler_(es_data, es_len);
    } else if (pid == audio_pid_ && audio_handler_) {
        return audio_handler_(es_data, es_len);
    }
    
    return true;
}

MemoryTSDemuxer::ProcessStats MemoryTSDemuxer::GetStats() const {
    return stats_;
}

// MemoryESOutput Implementation
TSDemuxerStreamManager::MemoryESOutput::MemoryESOutput(ES_OUTPUT_TYPE type, HANDLE pipe_handle)
    : type_(type), pipe_handle_(pipe_handle) {
}

TSDemuxerStreamManager::MemoryESOutput::~MemoryESOutput() {
}

bool TSDemuxerStreamManager::MemoryESOutput::WriteData(const unsigned char* data, unsigned int length) {
    if (pipe_handle_ == INVALID_HANDLE_VALUE || !data || length == 0) {
        return false;
    }
    
    DWORD bytes_written = 0;
    BOOL result = WriteFile(pipe_handle_, data, length, &bytes_written, nullptr);
    
    if (result && bytes_written == length) {
        bytes_written_ += bytes_written;
        return true;
    }
    
    return false;
}

// TSDemuxerStreamManager Implementation
TSDemuxerStreamManager::TSDemuxerStreamManager(const std::wstring& player_path, const std::wstring& channel_name)
    : player_path_(player_path), channel_name_(channel_name),
      player_process_(INVALID_HANDLE_VALUE), 
      video_pipe_(INVALID_HANDLE_VALUE), 
      audio_pipe_(INVALID_HANDLE_VALUE) {
    memset(&process_info_, 0, sizeof(process_info_));
}

TSDemuxerStreamManager::~TSDemuxerStreamManager() {
    StopStreaming();
    Cleanup();
}

bool TSDemuxerStreamManager::Initialize() {
    AddDebugLog(L"[TS_DEMUX] Initializing TS Demuxer stream manager for " + channel_name_);
    return true;
}

bool TSDemuxerStreamManager::StartStreaming(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    std::atomic<int>* chunk_count) {
    
    if (streaming_active_.load()) {
        return false;
    }
    
    log_callback_ = log_callback;
    chunk_count_ptr_ = chunk_count;
    cancel_token_ptr_ = &cancel_token;
    should_stop_ = false;
    
    // Create player with named pipes for video and audio
    if (!CreatePlayerWithPipes()) {
        LogMessage(L"Failed to create player with pipes");
        return false;
    }
    
    // Start streaming thread
    streaming_thread_ = std::thread(&TSDemuxerStreamManager::StreamingThreadFunction, this, playlist_url);
    streaming_active_ = true;
    
    LogMessage(L"TS Demuxer streaming started for " + channel_name_);
    return true;
}

void TSDemuxerStreamManager::StopStreaming() {
    if (streaming_active_.load()) {
        should_stop_ = true;
        streaming_active_ = false;
        
        if (streaming_thread_.joinable()) {
            streaming_thread_.join();
        }
        
        Cleanup();
        LogMessage(L"TS Demuxer streaming stopped for " + channel_name_);
    }
}

bool TSDemuxerStreamManager::CreatePlayerWithPipes() {
    // For now, use stdin mode like other streaming modes
    // In the future, we could implement separate named pipes for video/audio
    
    std::wstring player_args = L"-";
    std::wstring cmdline = L"\"" + player_path_ + L"\" " + player_args;
    
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    // Create pipe for stdin
    HANDLE read_pipe, write_pipe;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        AddDebugLog(L"[TS_DEMUX] Failed to create stdin pipe");
        return false;
    }
    
    si.hStdInput = read_pipe;
    video_pipe_ = write_pipe; // Use single pipe for now, interleave video/audio
    audio_pipe_ = write_pipe;
    
    wchar_t* cmdline_buf = _wcsdup(cmdline.c_str());
    BOOL result = CreateProcessW(nullptr, cmdline_buf, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &process_info_);
    free(cmdline_buf);
    
    CloseHandle(read_pipe);
    
    if (!result) {
        CloseHandle(write_pipe);
        AddDebugLog(L"[TS_DEMUX] Failed to create player process");
        return false;
    }
    
    player_process_ = process_info_.hProcess;
    return true;
}

void TSDemuxerStreamManager::StreamingThreadFunction(const std::wstring& playlist_url) {
    LogMessage(L"TS Demuxer streaming thread started");
    
    try {
        std::vector<std::wstring> segment_urls;
        
        while (!should_stop_.load() && !cancel_token_ptr_->load()) {
            // Download playlist and get segment URLs
            if (!DownloadPlaylistSegments(playlist_url, segment_urls)) {
                LogMessage(L"Failed to download playlist segments");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            // Process each segment
            for (const auto& segment_url : segment_urls) {
                if (should_stop_.load() || cancel_token_ptr_->load()) break;
                
                std::vector<char> segment_data;
                if (DownloadSegment(segment_url, segment_data)) {
                    if (ProcessSegmentWithDemuxer(segment_data)) {
                        segments_processed_++;
                        bytes_transferred_ += segment_data.size();
                        
                        if (chunk_count_ptr_) {
                            *chunk_count_ptr_ = static_cast<int>(segments_processed_.load());
                        }
                    }
                }
                
                // Small delay between segments
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            // Refresh playlist
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        LogMessage(L"Exception in streaming thread: " + Utf8ToWide(e.what()));
    }
    
    LogMessage(L"TS Demuxer streaming thread ended");
}

bool TSDemuxerStreamManager::DownloadPlaylistSegments(const std::wstring& playlist_url, std::vector<std::wstring>& segment_urls) {
    std::string playlist_data;
    if (!HttpGetText(playlist_url, playlist_data, cancel_token_ptr_)) {
        return false;
    }
    
    segment_urls.clear();
    std::istringstream iss(playlist_data);
    std::string line;
    
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        std::wstring segment_url = Utf8ToWide(line);
        segment_urls.push_back(JoinUrl(playlist_url, segment_url));
    }
    
    return !segment_urls.empty();
}

bool TSDemuxerStreamManager::DownloadSegment(const std::wstring& segment_url, std::vector<char>& segment_data) {
    return HttpGetBinary(segment_url, segment_data, cancel_token_ptr_);
}

bool TSDemuxerStreamManager::ProcessSegmentWithDemuxer(const std::vector<char>& segment_data) {
    if (segment_data.empty()) return false;
    
    MemoryTSDemuxer demuxer;
    if (!demuxer.Initialize(segment_data.data(), segment_data.size())) {
        return false;
    }
    
    // Set up output handlers
    demuxer.SetVideoOutput([this](const unsigned char* data, unsigned int length) -> bool {
        video_packets_++;
        if (video_output_) {
            return video_output_->WriteData(data, length);
        }
        // Write directly to pipe for now
        DWORD written;
        return WriteFile(video_pipe_, data, length, &written, nullptr) && written == length;
    });
    
    demuxer.SetAudioOutput([this](const unsigned char* data, unsigned int length) -> bool {
        audio_packets_++;
        if (audio_output_) {
            return audio_output_->WriteData(data, length);
        }
        // Write directly to pipe for now
        DWORD written;
        return WriteFile(audio_pipe_, data, length, &written, nullptr) && written == length;
    });
    
    return demuxer.Process();
}

void TSDemuxerStreamManager::LogMessage(const std::wstring& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}

void TSDemuxerStreamManager::UpdateChunkCount(int count) {
    if (chunk_count_ptr_) {
        *chunk_count_ptr_ = count;
    }
}

void TSDemuxerStreamManager::Cleanup() {
    if (video_pipe_ != INVALID_HANDLE_VALUE && video_pipe_ != audio_pipe_) {
        CloseHandle(video_pipe_);
    }
    if (audio_pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(audio_pipe_);
    }
    video_pipe_ = INVALID_HANDLE_VALUE;
    audio_pipe_ = INVALID_HANDLE_VALUE;
    
    if (player_process_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(player_process_, 0);
        CloseHandle(player_process_);
        player_process_ = INVALID_HANDLE_VALUE;
    }
    
    if (process_info_.hThread != nullptr) {
        CloseHandle(process_info_.hThread);
        process_info_.hThread = nullptr;
    }
}

TSDemuxerStreamManager::DemuxerStats TSDemuxerStreamManager::GetStats() const {
    DemuxerStats stats;
    stats.segments_processed = segments_processed_.load();
    stats.video_packets = video_packets_.load();
    stats.audio_packets = audio_packets_.load();
    stats.bytes_transferred = bytes_transferred_.load();
    stats.player_running = (player_process_ != INVALID_HANDLE_VALUE);
    stats.demuxer_active = streaming_active_.load();
    return stats;
}

// C++ wrapper function for integration with existing stream thread system
std::thread StartTSDemuxerThread(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    HWND main_window,
    size_t tab_index,
    HANDLE* player_process_handle) {
    
    return std::thread([=, &cancel_token]() mutable {
        if (log_callback) {
            log_callback(L"Starting TS Demuxer streaming thread for " + channel_name);
        }
        
        AddDebugLog(L"StartTSDemuxerThread: TS Demuxer mode - Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index));
        
        try {
            auto stream_manager = std::make_unique<TSDemuxerStreamManager>(player_path, channel_name);
            
            if (!stream_manager->Initialize()) {
                if (log_callback) {
                    log_callback(L"[TS_DEMUX] Failed to initialize TS Demuxer system");
                }
                return;
            }
            
            // Store player process handle if pointer provided
            if (player_process_handle) {
                *player_process_handle = stream_manager->GetPlayerProcess();
            }
            
            // Start streaming
            if (!stream_manager->StartStreaming(playlist_url, cancel_token, log_callback, chunk_count)) {
                if (log_callback) {
                    log_callback(L"[TS_DEMUX] Failed to start TS Demuxer streaming");
                }
                return;
            }
            
            if (log_callback) {
                log_callback(L"[TS_DEMUX] TS Demuxer streaming active for " + channel_name);
            }
            
            // Monitor streaming while active
            while (stream_manager->IsStreaming() && !cancel_token.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                
                auto stats = stream_manager->GetStats();
                
                // Update chunk count for status bar
                if (chunk_count) {
                    *chunk_count = static_cast<int>(stats.segments_processed);
                }
                
                // Periodic logging with statistics
                if (log_callback && stats.segments_processed % 10 == 0 && stats.segments_processed > 0) {
                    std::wstring status_msg = L"[TS_DEMUX] Segments: " + std::to_wstring(stats.segments_processed) + 
                                             L", Video: " + std::to_wstring(stats.video_packets) + 
                                             L", Audio: " + std::to_wstring(stats.audio_packets) + 
                                             L", Bytes: " + std::to_wstring(stats.bytes_transferred / 1024) + L"KB";
                    
                    if (!stats.player_running) {
                        status_msg += L" [PLAYER_DEAD]";
                    }
                    if (!stats.demuxer_active) {
                        status_msg += L" [DEMUX_INACTIVE]";
                    }
                    
                    log_callback(status_msg);
                }
                
                // Check if player died
                if (!stats.player_running) {
                    if (log_callback) {
                        log_callback(L"[TS_DEMUX] Player process died, stopping streaming");
                    }
                    break;
                }
            }
            
            // Stop streaming
            stream_manager->StopStreaming();
            
            if (log_callback) {
                log_callback(L"[TS_DEMUX] TS Demuxer streaming completed for " + channel_name);
            }
            
        } catch (const std::exception& e) {
            std::string error_msg = e.what();
            if (log_callback) {
                log_callback(L"[TS_DEMUX] Error: " + std::wstring(error_msg.begin(), error_msg.end()));
            }
            AddDebugLog(L"StartTSDemuxerThread: Exception: " + 
                       std::wstring(error_msg.begin(), error_msg.end()));
        }
        
        AddDebugLog(L"StartTSDemuxerThread: TS Demuxer stream finished, Channel=" + channel_name + 
                   L", Tab=" + std::to_wstring(tab_index));
        
        if (log_callback) {
            bool user_stopped = cancel_token.load();
            
            if (user_stopped) {
                log_callback(L"[TS_DEMUX] TS Demuxer streaming stopped by user.");
            } else {
                log_callback(L"[TS_DEMUX] TS Demuxer stream ended normally.");
                // Post auto-stop message for this specific tab
                if (main_window && tab_index != SIZE_MAX) {
                    AddDebugLog(L"StartTSDemuxerThread: Posting auto-stop for tab " + std::to_wstring(tab_index));
                    PostMessage(main_window, WM_USER + 2, (WPARAM)tab_index, 0);
                }
            }
        }
    });
}