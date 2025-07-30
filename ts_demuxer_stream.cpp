#include "ts_demuxer_stream.h"
#include "stream_thread.h"
#include "stream_pipe.h"
#include <sstream>
#include <iomanip>
#include <regex>
#include <chrono>
#include <algorithm>
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
    
    AddDebugLog(L"[TS_DEMUX] Initializing demuxer with " + std::to_wstring(buffer_size) + L" bytes");
    
    // Reset state
    pmt_pid_ = 0;
    pcr_pid_ = 0;
    video_pid_ = 0;
    audio_pid_ = 0;
    memset(&stats_, 0, sizeof(stats_));
    
    bool result = DetectPacketSize();
    if (!result) {
        AddDebugLog(L"[TS_DEMUX] Failed to detect packet size - stream may not be valid MPEG-TS");
    }
    return result;
}

bool MemoryTSDemuxer::DetectPacketSize() {
    if (!buffer_ || buffer_size_ < 1024) return false;
    
    // Look for sync bytes to determine packet size - require multiple consistent sync bytes
    for (size_t i = 0; i < std::min(buffer_size_, size_t(512)); i++) {
        if (buffer_[i] == 0x47) { // TS sync byte
            // Check for consistent sync bytes at 188, 192, or 204 byte intervals
            // Require at least 2-3 consecutive sync bytes for confidence
            int found_188 = 0, found_192 = 0, found_204 = 0;
            
            for (int check = 0; check < 3; check++) {
                size_t pos_188 = i + (check + 1) * 188;
                size_t pos_192 = i + (check + 1) * 192;
                size_t pos_204 = i + (check + 1) * 204;
                
                if (pos_188 < buffer_size_ && buffer_[pos_188] == 0x47) found_188++;
                if (pos_192 < buffer_size_ && buffer_[pos_192] == 0x47) found_192++;
                if (pos_204 < buffer_size_ && buffer_[pos_204] == 0x47) found_204++;
            }
            
            // Choose the packet size with the most consecutive sync bytes
            if (found_188 >= 2) {
                packet_size_ = 188;
                current_offset_ = i;
                AddDebugLog(L"[TS_DEMUX] Detected packet size: 188 bytes at offset " + std::to_wstring(i));
                return true;
            }
            if (found_192 >= 2) {
                packet_size_ = 192;
                current_offset_ = i;
                AddDebugLog(L"[TS_DEMUX] Detected packet size: 192 bytes at offset " + std::to_wstring(i));
                return true;
            }
            if (found_204 >= 2) {
                packet_size_ = 204;
                current_offset_ = i;
                AddDebugLog(L"[TS_DEMUX] Detected packet size: 204 bytes at offset " + std::to_wstring(i));
                return true;
            }
        }
    }
    
    AddDebugLog(L"[TS_DEMUX] Failed to detect consistent packet size in buffer of " + std::to_wstring(buffer_size_) + L" bytes");
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
            // Add more specific debugging information
            unsigned int pid = ((packet[1] & 0x1F) << 8) | packet[2];
            unsigned int field_ctrl = (packet[3] & 0x30) >> 4;
            bool unit_start = (packet[1] & 0x40) != 0;
            
            std::wstring debug_msg = L"[TS_DEMUX] Failed to parse TS packet - PID: 0x" + 
                                   std::to_wstring(pid) + L", Field: " + std::to_wstring(field_ctrl) + 
                                   L", Unit Start: " + (unit_start ? L"1" : L"0");
            AddDebugLog(debug_msg);
        } else {
            // Log successful parsing for first few packets and every 100th packet
            if (stats_.packets_processed < 10 || stats_.packets_processed % 100 == 0) {
                unsigned int pid = ((packet[1] & 0x1F) << 8) | packet[2];
                if (pid == 0x0000 || pid == pmt_pid_ || pid == video_pid_ || pid == audio_pid_) {
                    std::wstring debug_msg = L"[TS_DEMUX] Successfully parsed packet - PID: 0x" + 
                                           std::to_wstring(pid) + L" (PMT: " + std::to_wstring(pmt_pid_) + 
                                           L", Video: " + std::to_wstring(video_pid_) + 
                                           L", Audio: " + std::to_wstring(audio_pid_) + L")";
                    AddDebugLog(debug_msg);
                }
            }
        }
        
        stats_.packets_processed++;
        current_offset_ += packet_size_;
    }
    
    stats_.video_pid = video_pid_;
    stats_.audio_pid = audio_pid_;
    stats_.pcr_pid = pcr_pid_;
    stats_.has_video = (video_pid_ != 0);
    stats_.has_audio = (audio_pid_ != 0);
    
    // Log completion summary
    AddDebugLog(L"[TS_DEMUX] Processing complete - Packets: " + std::to_wstring(stats_.packets_processed) + 
               L", Video PID: 0x" + std::to_wstring(video_pid_) + 
               L", Audio PID: 0x" + std::to_wstring(audio_pid_) + 
               L", PMT PID: 0x" + std::to_wstring(pmt_pid_));
    
    return true;
}

bool MemoryTSDemuxer::ParseTSPacket(const unsigned char* packet) {
    // Parse TS header
    bool transport_error = (packet[1] & 0x80) != 0;
    bool unit_start = (packet[1] & 0x40) != 0;
    unsigned int pid = ((packet[1] & 0x1F) << 8) | packet[2];
    unsigned int field_ctrl = (packet[3] & 0x30) >> 4;
    unsigned int continuity = packet[3] & 0x0F;
    
    // Skip packets with transport errors
    if (transport_error) {
        return true;
    }
    
    const unsigned char* payload = nullptr;
    unsigned int payload_len = 0;
    
    switch (field_ctrl) {
        case 0: // Reserved for future use - treat as no payload
            return true;
            
        case 1: // Payload only
            payload = packet + 4;
            payload_len = packet_size_ - 4;
            break;
            
        case 2: // Adaptation field only, no payload
            // Process adaptation field if needed, but no payload to parse
            return true;
            
        case 3: // Adaptation field + payload
            if (packet_size_ > 5) {
                unsigned int adapt_len = packet[4];
                if (adapt_len < packet_size_ - 5) {
                    payload = packet + 5 + adapt_len;
                    payload_len = packet_size_ - 5 - adapt_len;
                }
            }
            break;
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
    if (length < 8) return true; // Not enough data, but don't fail
    
    unsigned int pointer = payload[0];
    if (pointer >= length) return true; // Skip malformed pointer
    
    const unsigned char* table = payload + 1 + pointer;
    unsigned int table_len = length - 1 - pointer;
    
    if (table_len < 8) return true; // Not enough data for table
    
    // Verify table ID is PAT (0x00)
    if (table[0] != 0x00) return true; // Not a PAT
    
    unsigned int section_length = ((table[1] & 0x03) << 8) | table[2];
    if (section_length < 4 || section_length + 3 > table_len) return true; // Malformed section
    
    // Skip to program list (after table header)
    const unsigned char* programs = table + 8;
    unsigned int programs_len = section_length - 9; // Exclude CRC32
    
    for (unsigned int i = 0; i + 3 < programs_len; i += 4) {
        unsigned int program_num = (programs[i] << 8) | programs[i + 1];
        unsigned int program_pid = ((programs[i + 2] & 0x1F) << 8) | programs[i + 3];
        
        if (program_num != 0) { // Skip NIT PID
            pmt_pid_ = program_pid;
            AddDebugLog(L"[TS_DEMUX] Found PMT PID: 0x" + std::to_wstring(program_pid) + 
                       L" for program " + std::to_wstring(program_num));
            break;
        }
    }
    
    return true;
}

bool MemoryTSDemuxer::ParsePMT(const unsigned char* payload, unsigned int length) {
    if (length < 8) return true; // Not enough data, but don't fail
    
    unsigned int pointer = payload[0];
    if (pointer >= length) return true; // Skip malformed pointer
    
    const unsigned char* table = payload + 1 + pointer;
    unsigned int table_len = length - 1 - pointer;
    
    if (table_len < 12) return true; // Not enough data for PMT table
    
    // Verify table ID is PMT (0x02)
    if (table[0] != 0x02) return true; // Not a PMT
    
    unsigned int section_length = ((table[1] & 0x03) << 8) | table[2];
    if (section_length < 4 || section_length + 3 > table_len) return true; // Malformed section
    
    pcr_pid_ = ((table[8] & 0x1F) << 8) | table[9];
    unsigned int prog_info_len = ((table[10] & 0x03) << 8) | table[11];
    
    // Bounds check for program info length
    if (prog_info_len + 13 > section_length) return true;
    
    // Skip to elementary streams
    const unsigned char* streams = table + 12 + prog_info_len;
    unsigned int streams_len = section_length - 13 - prog_info_len; // Exclude CRC32
    
    for (unsigned int i = 0; i + 4 < streams_len;) {
        unsigned int stream_type = streams[i];
        unsigned int stream_pid = ((streams[i + 1] & 0x1F) << 8) | streams[i + 2];
        unsigned int es_info_len = ((streams[i + 3] & 0x03) << 8) | streams[i + 4];
        
        // Bounds check for ES info length
        if (i + 5 + es_info_len > streams_len) break;
        
        // Identify video and audio streams - support more formats
        bool is_video_type = (stream_type == 0x1B ||  // H.264/AVC
                              stream_type == 0x24 ||  // H.265/HEVC 
                              stream_type == 0x27 ||  // H.265/HEVC (alternate)
                              stream_type == 0x02);   // MPEG-2 Video
                              
        bool is_audio_type = (stream_type == 0x0F ||  // ADTS AAC
                              stream_type == 0x11 ||  // LATM AAC
                              stream_type == 0x15 ||  // AAC elementary stream
                              stream_type == 0x03 ||  // MPEG-1 Audio
                              stream_type == 0x04);   // MPEG-2 Audio
                              
        if (is_video_type && video_pid_ == 0) {
            video_pid_ = stream_pid;
            AddDebugLog(L"[TS_DEMUX] Found video stream - Type: 0x" + std::to_wstring(stream_type) + 
                       L", PID: 0x" + std::to_wstring(stream_pid) + L" (ASSIGNED)");
        } else if (is_audio_type && audio_pid_ == 0) {
            audio_pid_ = stream_pid;
            AddDebugLog(L"[TS_DEMUX] Found audio stream - Type: 0x" + std::to_wstring(stream_type) + 
                       L", PID: 0x" + std::to_wstring(stream_pid) + L" (ASSIGNED)");
        } else if (!is_video_type && !is_audio_type) {
            AddDebugLog(L"[TS_DEMUX] Ignoring unknown stream type 0x" + std::to_wstring(stream_type) + 
                       L", PID: 0x" + std::to_wstring(stream_pid));
        }
        
        i += 5 + es_info_len;
    }
    
    return true;
}

bool MemoryTSDemuxer::ParsePES(const unsigned char* payload, unsigned int length, unsigned int pid, bool unit_start) {
    if (!unit_start || length < 3) {
        // Continue with existing PES packet or insufficient data
        if (pid == video_pid_ && video_handler_ && length > 0) {
            static int video_continuation_count = 0;
            if (++video_continuation_count <= 3) {
                AddDebugLog(L"[TS_DEMUX] Writing video continuation data - " + std::to_wstring(length) + L" bytes");
            }
            video_handler_(payload, length); // Don't fail packet parsing if write fails
        } else if (pid == audio_pid_ && audio_handler_ && length > 0) {
            static int audio_continuation_count = 0;
            if (++audio_continuation_count <= 3) {
                AddDebugLog(L"[TS_DEMUX] Writing audio continuation data - " + std::to_wstring(length) + L" bytes");
            }
            audio_handler_(payload, length); // Don't fail packet parsing if write fails
        }
        return true;
    }
    
    // Check for PES start code (more flexible for real streams)
    if (length >= 3 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
        // Valid PES start code - parse header
        unsigned int pes_header_len = 6;
        if (length > 8 && payload[6] == 0x80) { // Check PTS/DTS flags exist
            pes_header_len += 3 + payload[8]; // Additional header fields
        }
        
        if (pes_header_len < length) {
            const unsigned char* es_data = payload + pes_header_len;
            unsigned int es_len = length - pes_header_len;
            
            if (pid == video_pid_ && video_handler_) {
                static int video_pes_count = 0;
                if (++video_pes_count <= 3) {
                    AddDebugLog(L"[TS_DEMUX] Writing video PES data - " + std::to_wstring(es_len) + L" bytes");
                }
                video_handler_(es_data, es_len); // Don't fail packet parsing if write fails
            } else if (pid == audio_pid_ && audio_handler_) {
                static int audio_pes_count = 0;
                if (++audio_pes_count <= 3) {
                    AddDebugLog(L"[TS_DEMUX] Writing audio PES data - " + std::to_wstring(es_len) + L" bytes");
                }
                audio_handler_(es_data, es_len); // Don't fail packet parsing if write fails
            }
        }
    } else {
        // No PES start code - treat as raw elementary stream data
        if (pid == video_pid_ && video_handler_) {
            static int video_raw_count = 0;
            if (++video_raw_count <= 3) {
                AddDebugLog(L"[TS_DEMUX] Writing raw video data - " + std::to_wstring(length) + L" bytes");
            }
            video_handler_(payload, length); // Don't fail packet parsing if write fails
        } else if (pid == audio_pid_ && audio_handler_) {
            static int audio_raw_count = 0;
            if (++audio_raw_count <= 3) {
                AddDebugLog(L"[TS_DEMUX] Writing raw audio data - " + std::to_wstring(length) + L" bytes");
            }
            audio_handler_(payload, length); // Don't fail packet parsing if write fails
        }
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
    
    // Log specific error if write fails
    DWORD error = GetLastError();
    static int error_count = 0;
    if (++error_count <= 5) { // Only log first 5 errors to avoid spam
        AddDebugLog(L"[TS_DEMUX] WriteFile failed for " + 
                   std::wstring(type_ == ES_OUTPUT_VIDEO ? L"video" : L"audio") + 
                   L" pipe, error: " + std::to_wstring(error) + 
                   L", bytes written: " + std::to_wstring(bytes_written) + 
                   L"/" + std::to_wstring(length));
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
    CleanupNamedPipes();
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
    
    // Create named pipes first but don't start player yet
    if (!CreateNamedPipes()) {
        LogMessage(L"Failed to create named pipes for video/audio separation");
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

bool TSDemuxerStreamManager::CreateNamedPipes() {
    // Generate unique pipe names using process ID and timestamp
    DWORD pid = GetCurrentProcessId();
    DWORD timestamp = GetTickCount();
    
    video_pipe_path_ = L"\\\\.\\pipe\\tardsplaya_video_" + std::to_wstring(pid) + L"_" + std::to_wstring(timestamp);
    audio_pipe_path_ = L"\\\\.\\pipe\\tardsplaya_audio_" + std::to_wstring(pid) + L"_" + std::to_wstring(timestamp);
    
    // Create video named pipe
    video_pipe_ = CreateNamedPipeW(
        video_pipe_path_.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, // max instances
        65536, // output buffer size
        65536, // input buffer size
        0, // default timeout
        nullptr // default security
    );
    
    if (video_pipe_ == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"[TS_DEMUX] Failed to create video named pipe: " + video_pipe_path_ + L", error: " + std::to_wstring(error));
        return false;
    }
    
    // Create audio named pipe
    audio_pipe_ = CreateNamedPipeW(
        audio_pipe_path_.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, // max instances
        65536, // output buffer size
        65536, // input buffer size
        0, // default timeout
        nullptr // default security
    );
    
    if (audio_pipe_ == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(video_pipe_);
        video_pipe_ = INVALID_HANDLE_VALUE;
        AddDebugLog(L"[TS_DEMUX] Failed to create audio named pipe: " + audio_pipe_path_ + L", error: " + std::to_wstring(error));
        return false;
    }
    
    // Create MemoryESOutput objects immediately after pipes are created
    // This allows writing to pipes even before player connects
    video_output_ = std::make_unique<MemoryESOutput>(ES_OUTPUT_VIDEO, video_pipe_);
    audio_output_ = std::make_unique<MemoryESOutput>(ES_OUTPUT_AUDIO, audio_pipe_);
    
    AddDebugLog(L"[TS_DEMUX] Created named pipes - Video: " + video_pipe_path_ + L", Audio: " + audio_pipe_path_);
    AddDebugLog(L"[TS_DEMUX] Created output objects for immediate data writing");
    return true;
}

void TSDemuxerStreamManager::CleanupNamedPipes() {
    // Clean up MemoryESOutput objects first
    video_output_.reset();
    audio_output_.reset();
    
    if (video_pipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(video_pipe_);
        CloseHandle(video_pipe_);
        video_pipe_ = INVALID_HANDLE_VALUE;
        AddDebugLog(L"[TS_DEMUX] Closed video named pipe: " + video_pipe_path_);
    }
    
    if (audio_pipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(audio_pipe_);
        CloseHandle(audio_pipe_);
        audio_pipe_ = INVALID_HANDLE_VALUE;
        AddDebugLog(L"[TS_DEMUX] Closed audio named pipe: " + audio_pipe_path_);
    }
    
    video_pipe_path_.clear();
    audio_pipe_path_.clear();
}

bool TSDemuxerStreamManager::StartPlayerWithPipes() {
    // Named pipes should already be created by CreateNamedPipes()
    if (video_pipe_ == INVALID_HANDLE_VALUE || audio_pipe_ == INVALID_HANDLE_VALUE) {
        LogMessage(L"Named pipes not ready for player start");
        return false;
    }
    
    // Put named pipes in listening state FIRST, before starting the player
    AddDebugLog(L"[TS_DEMUX] Putting named pipes in listening state for player connection...");
    
    // Start listening for connections on both pipes in non-blocking mode
    // This ensures the pipes are ready when the player tries to access them
    OVERLAPPED video_overlapped = {0};
    OVERLAPPED audio_overlapped = {0};
    
    video_overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    audio_overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    
    if (!video_overlapped.hEvent || !audio_overlapped.hEvent) {
        AddDebugLog(L"[TS_DEMUX] Failed to create events for overlapped pipe operations");
        if (video_overlapped.hEvent) CloseHandle(video_overlapped.hEvent);
        if (audio_overlapped.hEvent) CloseHandle(audio_overlapped.hEvent);
        return false;
    }
    
    // Start non-blocking connect operations
    BOOL video_connect = ConnectNamedPipe(video_pipe_, &video_overlapped);
    BOOL audio_connect = ConnectNamedPipe(audio_pipe_, &audio_overlapped);
    
    DWORD video_error = GetLastError();
    DWORD audio_error = GetLastError();
    
    // Check if connections are pending or already connected
    if (!video_connect && video_error != ERROR_IO_PENDING && video_error != ERROR_PIPE_CONNECTED) {
        AddDebugLog(L"[TS_DEMUX] Failed to start video pipe connection, error: " + std::to_wstring(video_error));
        CloseHandle(video_overlapped.hEvent);
        CloseHandle(audio_overlapped.hEvent);
        return false;
    }
    
    if (!audio_connect && audio_error != ERROR_IO_PENDING && audio_error != ERROR_PIPE_CONNECTED) {
        AddDebugLog(L"[TS_DEMUX] Failed to start audio pipe connection, error: " + std::to_wstring(audio_error));
        CloseHandle(video_overlapped.hEvent);
        CloseHandle(audio_overlapped.hEvent);
        return false;
    }
    
    AddDebugLog(L"[TS_DEMUX] Named pipes are now ready for client connections");
    
    // Now start the player with command line pointing to the named pipes
    std::wstring player_name = player_path_;
    std::transform(player_name.begin(), player_name.end(), player_name.begin(), ::towlower);
    
    std::wstring cmdline;
    if (player_name.find(L"mpv") != std::wstring::npos) {
        // MPV supports --audio-file= for separate audio track
        cmdline = L"\"" + player_path_ + L"\" --audio-file=\"" + audio_pipe_path_ + L"\" \"" + video_pipe_path_ + L"\"";
    } else if (player_name.find(L"mpc") != std::wstring::npos || player_name.find(L"mphc") != std::wstring::npos) {
        // MPC-HC supports /dub for separate audio track  
        cmdline = L"\"" + player_path_ + L"\" /dub \"" + audio_pipe_path_ + L"\" \"" + video_pipe_path_ + L"\"";
    } else {
        // Default: try MPV syntax
        cmdline = L"\"" + player_path_ + L"\" --audio-file=\"" + audio_pipe_path_ + L"\" \"" + video_pipe_path_ + L"\"";
    }
    
    // Validate player path exists before trying to start it
    if (GetFileAttributesW(player_path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        AddDebugLog(L"[TS_DEMUX] Player executable not found: " + player_path_);
        CloseHandle(video_overlapped.hEvent);
        CloseHandle(audio_overlapped.hEvent);
        return false;
    }
    
    AddDebugLog(L"[TS_DEMUX] Player command: " + cmdline);
    
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    
    wchar_t* cmdline_buf = _wcsdup(cmdline.c_str());
    BOOL result = CreateProcessW(nullptr, cmdline_buf, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &process_info_);
    free(cmdline_buf);
    
    if (!result) {
        DWORD error = GetLastError();
        AddDebugLog(L"[TS_DEMUX] Failed to create player process, error: " + std::to_wstring(error));
        CloseHandle(video_overlapped.hEvent);
        CloseHandle(audio_overlapped.hEvent);
        return false;
    }
    
    player_process_ = process_info_.hProcess;
    AddDebugLog(L"[TS_DEMUX] Player process started with PID: " + std::to_wstring(process_info_.dwProcessId));
    
    // Check if player process is still running immediately after creation
    DWORD exit_code;
    if (GetExitCodeProcess(player_process_, &exit_code)) {
        if (exit_code != STILL_ACTIVE) {
            AddDebugLog(L"[TS_DEMUX] Player process exited immediately with code: " + std::to_wstring(exit_code));
            CloseHandle(video_overlapped.hEvent);
            CloseHandle(audio_overlapped.hEvent);
            return false;
        }
    }
    
    AddDebugLog(L"[TS_DEMUX] Player process running, waiting for pipe connections...");
    
    // Give the player a moment to initialize before expecting pipe connections
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Check again if player is still running after initialization delay
    if (GetExitCodeProcess(player_process_, &exit_code)) {
        if (exit_code != STILL_ACTIVE) {
            AddDebugLog(L"[TS_DEMUX] Player process died during initialization, exit code: " + std::to_wstring(exit_code));
            CloseHandle(video_overlapped.hEvent);
            CloseHandle(audio_overlapped.hEvent);
            return false;
        }
    }
    
    // Wait for the player to actually connect to both pipes (with timeout)
    HANDLE events[2] = { video_overlapped.hEvent, audio_overlapped.hEvent };
    DWORD wait_result = WaitForMultipleObjects(2, events, TRUE, 10000); // 10 second timeout
    
    if (wait_result == WAIT_TIMEOUT) {
        AddDebugLog(L"[TS_DEMUX] Timeout waiting for player to connect to named pipes");
        
        // Check if player is still running
        DWORD exit_code;
        if (GetExitCodeProcess(player_process_, &exit_code)) {
            if (exit_code != STILL_ACTIVE) {
                AddDebugLog(L"[TS_DEMUX] Player process died during pipe connection wait, exit code: " + std::to_wstring(exit_code));
            } else {
                AddDebugLog(L"[TS_DEMUX] Player process still running but didn't connect to pipes");
            }
        }
        
        CloseHandle(video_overlapped.hEvent);
        CloseHandle(audio_overlapped.hEvent);
        return false;
    } else if (wait_result == WAIT_FAILED) {
        DWORD error = GetLastError();
        AddDebugLog(L"[TS_DEMUX] Failed waiting for pipe connections, error: " + std::to_wstring(error));
        CloseHandle(video_overlapped.hEvent);
        CloseHandle(audio_overlapped.hEvent);
        return false;
    }
    
    // Clean up events
    CloseHandle(video_overlapped.hEvent);
    CloseHandle(audio_overlapped.hEvent);
    
    LogMessage(L"TS Demuxer player started with named pipes for video/audio streaming");
    AddDebugLog(L"[TS_DEMUX] Named pipes connected successfully to player");
    return true;
}



void TSDemuxerStreamManager::StreamingThreadFunction(const std::wstring& playlist_url) {
    LogMessage(L"TS Demuxer streaming thread started");
    
    try {
        std::vector<std::wstring> segment_urls;
        bool player_started = false;
        
        // Start player immediately and wait for connection
        LogMessage(L"Starting player before processing segments to ensure pipe connections");
        if (StartPlayerWithPipes()) {
            player_started = true;
            LogMessage(L"Player started successfully with named pipes for video/audio streaming");
        } else {
            LogMessage(L"Failed to start player - aborting TS Demuxer streaming");
            return;
        }
        
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
                
                // Reduced delay between segments for better performance - was 100ms, now 25ms
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            
            // Refresh playlist - reduced from 1 second to 500ms for more responsive streaming
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    
    // Set up output handlers to write directly to named pipes
    demuxer.SetVideoOutput([this](const unsigned char* data, unsigned int length) -> bool {
        video_packets_++;
        if (video_output_) {
            return video_output_->WriteData(data, length);
        }
        AddDebugLog(L"[TS_DEMUX] Warning: Video output not initialized");
        return false;
    });
    
    demuxer.SetAudioOutput([this](const unsigned char* data, unsigned int length) -> bool {
        audio_packets_++;
        if (audio_output_) {
            return audio_output_->WriteData(data, length);
        }
        AddDebugLog(L"[TS_DEMUX] Warning: Audio output not initialized");
        return false;
    });
    
    bool result = demuxer.Process();
    return result;
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
    
    // Clean up named pipes
    CleanupNamedPipes();
}

TSDemuxerStreamManager::DemuxerStats TSDemuxerStreamManager::GetStats() const {
    DemuxerStats stats;
    stats.segments_processed = segments_processed_.load();
    stats.video_packets = video_packets_.load();
    stats.audio_packets = audio_packets_.load();
    stats.bytes_transferred = bytes_transferred_.load();
    
    // Check if player process is actually running, not just if handle is valid
    stats.player_running = false;
    if (player_process_ != INVALID_HANDLE_VALUE && player_process_ != nullptr) {
        DWORD exit_code;
        if (GetExitCodeProcess(player_process_, &exit_code)) {
            stats.player_running = (exit_code == STILL_ACTIVE);
        }
    }
    
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