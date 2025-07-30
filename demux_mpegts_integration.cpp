#include "demux_mpegts_integration.h"
#include "stream_thread.h"
#include <sstream>
#include <iomanip>
#include <regex>
#include <chrono>
#include <winhttp.h>
#include <shellapi.h>
#include <filesystem>

// Include existing utility functions
extern std::string WideToUtf8(const std::wstring& w);
extern std::wstring Utf8ToWide(const std::string& s);
extern bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token);

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

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

// MpegTSDemuxer Implementation
MpegTSDemuxer::MpegTSDemuxer(const std::wstring& channel_name, const std::wstring& output_dir)
    : channel_name_(channel_name)
    , output_dir_(output_dir)
    , av_context_(nullptr)
    , buffer_(nullptr)
    , buffer_size_(0)
    , buffer_pos_(0)
    , stream_pos_(0)
    , primary_video_pid_(0xFFFF)
    , primary_audio_pid_(0xFFFF)
    , is_active_(false)
    , total_bytes_processed_(0)
{
    AddDebugLog(L"[DEMUX] Creating MPEG-TS demuxer for channel: " + channel_name_);
    
    // Set demux debug level
    TSDemux::DBGLevel(TSDemux::DEMUX_DBG_INFO);
}

MpegTSDemuxer::~MpegTSDemuxer() {
    Stop();
    AddDebugLog(L"[DEMUX] MPEG-TS demuxer destroyed");
}

bool MpegTSDemuxer::Initialize(size_t buffer_size) {
    AddDebugLog(L"[DEMUX] Initializing demuxer with buffer size: " + std::to_wstring(buffer_size));
    
    buffer_size_ = buffer_size;
    buffer_ = std::make_unique<unsigned char[]>(buffer_size_);
    if (!buffer_) {
        AddDebugLog(L"[DEMUX] ERROR: Failed to allocate buffer");
        return false;
    }
    
    buffer_pos_ = 0;
    stream_pos_ = 0;
    
    // Create AV context
    av_context_ = std::make_unique<TSDemux::AVContext>(this, 0, 0);
    if (!av_context_) {
        AddDebugLog(L"[DEMUX] ERROR: Failed to create AV context");
        return false;
    }
    
    is_active_ = true;
    AddDebugLog(L"[DEMUX] Demuxer initialized successfully");
    return true;
}

const unsigned char* MpegTSDemuxer::ReadAV(uint64_t pos, size_t len) {
    // Return data from our buffer
    if (pos >= stream_pos_ && (pos + len) <= (stream_pos_ + buffer_pos_)) {
        size_t offset = static_cast<size_t>(pos - stream_pos_);
        return buffer_.get() + offset;
    }
    return nullptr;
}

bool MpegTSDemuxer::ProcessTSData(const char* data, size_t size) {
    if (!is_active_ || !data || size == 0) {
        return false;
    }
    
    // Copy data to buffer
    if ((buffer_pos_ + size) > buffer_size_) {
        // Buffer overflow - shift existing data and add new data
        size_t keep_size = buffer_size_ / 2;
        if (buffer_pos_ > keep_size) {
            memmove(buffer_.get(), buffer_.get() + (buffer_pos_ - keep_size), keep_size);
            stream_pos_ += (buffer_pos_ - keep_size);
            buffer_pos_ = keep_size;
        }
    }
    
    if ((buffer_pos_ + size) <= buffer_size_) {
        memcpy(buffer_.get() + buffer_pos_, data, size);
        buffer_pos_ += size;
        total_bytes_processed_ += size;
    }
    
    // Process the data through AVContext
    if (av_context_) {
        try {
            // Configure the context if needed
            if (!av_context_->is_configured) {
                int ret = av_context_->TSResync();
                if (ret != TSDemux::AVCONTEXT_CONTINUE) {
                    av_context_->is_configured = true;
                    AddDebugLog(L"[DEMUX] TS stream configured");
                }
            }
            
            // Process packets
            while (buffer_pos_ >= FLUTS_NORMAL_TS_PACKETSIZE) {
                int ret = av_context_->ProcessTSPacket();
                if (ret == TSDemux::AVCONTEXT_CONTINUE) {
                    // Success - check for new elementary streams
                    for (auto& stream_pair : av_context_->GetStreams()) {
                        uint16_t pid = stream_pair.first;
                        TSDemux::ElementaryStream* es = stream_pair.second;
                        
                        if (streams_.find(pid) == streams_.end()) {
                            CreateOutputStream(pid, es->stream_type);
                        }
                        
                        // Get stream data
                        const unsigned char* stream_data = nullptr;
                        size_t stream_size = 0;
                        if (es->GetStreamData(stream_data, stream_size) && stream_data && stream_size > 0) {
                            OnStreamData(pid, es->stream_type, stream_data, stream_size);
                        }
                    }
                    
                    // Advance stream position
                    stream_pos_ += FLUTS_NORMAL_TS_PACKETSIZE;
                    buffer_pos_ -= FLUTS_NORMAL_TS_PACKETSIZE;
                    if (buffer_pos_ > 0) {
                        memmove(buffer_.get(), buffer_.get() + FLUTS_NORMAL_TS_PACKETSIZE, buffer_pos_);
                    }
                } else {
                    // Error or need more data
                    break;
                }
            }
        } catch (const std::exception& e) {
            AddDebugLog(L"[DEMUX] Exception during TS processing: " + Utf8ToWide(e.what()));
            return false;
        }
    }
    
    return true;
}

void MpegTSDemuxer::OnStreamData(uint16_t pid, TSDemux::STREAM_TYPE stream_type, 
                                 const unsigned char* data, size_t size) {
    auto it = streams_.find(pid);
    if (it != streams_.end() && it->second.output_file.is_open()) {
        it->second.output_file.write(reinterpret_cast<const char*>(data), size);
        it->second.bytes_written += size;
        
        // Log progress occasionally
        if ((it->second.bytes_written % (1024 * 1024)) == 0) { // Every 1MB
            AddDebugLog(L"[DEMUX] Written " + std::to_wstring(it->second.bytes_written / 1024) + 
                       L"KB to " + it->second.output_filename);
        }
    }
}

void MpegTSDemuxer::CreateOutputStream(uint16_t pid, TSDemux::STREAM_TYPE stream_type) {
    AddDebugLog(L"[DEMUX] Creating output stream for PID " + std::to_wstring(pid) + 
               L", type: " + std::to_wstring(static_cast<int>(stream_type)));
    
    ElementaryStreamData& stream = streams_[pid];
    stream.pid = pid;
    stream.stream_type = stream_type;
    stream.codec_name = GetCodecName(stream_type);
    stream.file_extension = GetFileExtension(stream_type);
    stream.output_filename = GenerateOutputFilename(pid, stream.file_extension);
    
    // Determine if this is video or audio
    stream.is_video = (stream_type == TSDemux::STREAM_TYPE_VIDEO_MPEG1 ||
                      stream_type == TSDemux::STREAM_TYPE_VIDEO_MPEG2 ||
                      stream_type == TSDemux::STREAM_TYPE_VIDEO_H264 ||
                      stream_type == TSDemux::STREAM_TYPE_VIDEO_HEVC);
    
    stream.is_audio = (stream_type == TSDemux::STREAM_TYPE_AUDIO_MPEG1 ||
                      stream_type == TSDemux::STREAM_TYPE_AUDIO_MPEG2 ||
                      stream_type == TSDemux::STREAM_TYPE_AUDIO_AAC ||
                      stream_type == TSDemux::STREAM_TYPE_AUDIO_AC3);
    
    // Open output file
    std::string narrow_filename = WideToUtf8(stream.output_filename);
    stream.output_file.open(narrow_filename, std::ios::binary | std::ios::out);
    
    if (stream.output_file.is_open()) {
        AddDebugLog(L"[DEMUX] Created output file: " + stream.output_filename);
        
        // Update primary stream selections
        SelectPrimaryStreams();
    } else {
        AddDebugLog(L"[DEMUX] ERROR: Failed to create output file: " + stream.output_filename);
    }
}

std::wstring MpegTSDemuxer::GetCodecName(TSDemux::STREAM_TYPE stream_type) {
    switch (stream_type) {
        case TSDemux::STREAM_TYPE_VIDEO_MPEG1: return L"MPEG-1 Video";
        case TSDemux::STREAM_TYPE_VIDEO_MPEG2: return L"MPEG-2 Video";
        case TSDemux::STREAM_TYPE_VIDEO_H264: return L"H.264";
        case TSDemux::STREAM_TYPE_VIDEO_HEVC: return L"H.265/HEVC";
        case TSDemux::STREAM_TYPE_AUDIO_MPEG1: return L"MPEG-1 Audio";
        case TSDemux::STREAM_TYPE_AUDIO_MPEG2: return L"MPEG-2 Audio";
        case TSDemux::STREAM_TYPE_AUDIO_AAC: return L"AAC";
        case TSDemux::STREAM_TYPE_AUDIO_AC3: return L"AC-3";
        default: return L"Unknown";
    }
}

std::wstring MpegTSDemuxer::GetFileExtension(TSDemux::STREAM_TYPE stream_type) {
    switch (stream_type) {
        case TSDemux::STREAM_TYPE_VIDEO_MPEG1:
        case TSDemux::STREAM_TYPE_VIDEO_MPEG2: return L"m2v";
        case TSDemux::STREAM_TYPE_VIDEO_H264: return L"h264";
        case TSDemux::STREAM_TYPE_VIDEO_HEVC: return L"h265";
        case TSDemux::STREAM_TYPE_AUDIO_MPEG1:
        case TSDemux::STREAM_TYPE_AUDIO_MPEG2: return L"mp3";
        case TSDemux::STREAM_TYPE_AUDIO_AAC: return L"aac";
        case TSDemux::STREAM_TYPE_AUDIO_AC3: return L"ac3";
        default: return L"raw";
    }
}

std::wstring MpegTSDemuxer::GenerateOutputFilename(uint16_t pid, const std::wstring& extension) {
    std::wostringstream filename;
    filename << output_dir_ << L"\\stream_" << channel_name_ << L"_" 
             << std::setfill(L'0') << std::setw(4) << pid << L"." << extension;
    return filename.str();
}

void MpegTSDemuxer::SelectPrimaryStreams() {
    // Select first video and audio streams as primary
    if (primary_video_pid_ == 0xFFFF) {
        for (const auto& stream_pair : streams_) {
            if (stream_pair.second.is_video) {
                primary_video_pid_ = stream_pair.first;
                video_output_file_ = stream_pair.second.output_filename;
                AddDebugLog(L"[DEMUX] Selected primary video stream PID " + std::to_wstring(primary_video_pid_));
                break;
            }
        }
    }
    
    if (primary_audio_pid_ == 0xFFFF) {
        for (const auto& stream_pair : streams_) {
            if (stream_pair.second.is_audio) {
                primary_audio_pid_ = stream_pair.first;
                audio_output_file_ = stream_pair.second.output_filename;
                AddDebugLog(L"[DEMUX] Selected primary audio stream PID " + std::to_wstring(primary_audio_pid_));
                break;
            }
        }
    }
}

void MpegTSDemuxer::Stop() {
    if (is_active_) {
        is_active_ = false;
        
        // Close all output files
        for (auto& stream_pair : streams_) {
            if (stream_pair.second.output_file.is_open()) {
                stream_pair.second.output_file.close();
                AddDebugLog(L"[DEMUX] Closed output file for PID " + std::to_wstring(stream_pair.first));
            }
        }
        
        AddDebugLog(L"[DEMUX] Demuxer stopped");
    }
}

MpegTSDemuxer::DemuxStats MpegTSDemuxer::GetStats() const {
    DemuxStats stats;
    stats.total_bytes_processed = total_bytes_processed_;
    stats.primary_video_pid = primary_video_pid_;
    stats.primary_audio_pid = primary_audio_pid_;
    stats.video_streams_count = 0;
    stats.audio_streams_count = 0;
    stats.video_bytes_written = 0;
    stats.audio_bytes_written = 0;
    
    for (const auto& stream_pair : streams_) {
        if (stream_pair.second.is_video) {
            stats.video_streams_count++;
            if (stream_pair.first == primary_video_pid_) {
                stats.video_bytes_written = stream_pair.second.bytes_written;
            }
        } else if (stream_pair.second.is_audio) {
            stats.audio_streams_count++;
            if (stream_pair.first == primary_audio_pid_) {
                stats.audio_bytes_written = stream_pair.second.bytes_written;
            }
        }
    }
    
    return stats;
}

// DemuxStreamManager Implementation
DemuxStreamManager::DemuxStreamManager(const std::wstring& player_path, const std::wstring& channel_name)
    : player_path_(player_path)
    , channel_name_(channel_name)
    , demuxer_(nullptr)
    , player_process_(nullptr)
    , log_callback_(nullptr)
    , chunk_count_ptr_(nullptr)
    , cancel_token_ptr_(nullptr)
{
    memset(&process_info_, 0, sizeof(process_info_));
    AddDebugLog(L"[DEMUX] Creating demux stream manager for " + channel_name_);
}

DemuxStreamManager::~DemuxStreamManager() {
    StopStreaming();
    AddDebugLog(L"[DEMUX] Demux stream manager destroyed");
}

bool DemuxStreamManager::Initialize() {
    AddDebugLog(L"[DEMUX] Initializing demux stream manager");
    
    // Create unique output directory
    output_dir_ = GenerateUniqueOutputDirectory();
    if (!CreateOutputDirectory()) {
        AddDebugLog(L"[DEMUX] ERROR: Failed to create output directory: " + output_dir_);
        return false;
    }
    
    // Create demuxer
    demuxer_ = std::make_unique<MpegTSDemuxer>(channel_name_, output_dir_);
    if (!demuxer_ || !demuxer_->Initialize()) {
        AddDebugLog(L"[DEMUX] ERROR: Failed to initialize demuxer");
        return false;
    }
    
    AddDebugLog(L"[DEMUX] Demux stream manager initialized successfully");
    return true;
}

bool DemuxStreamManager::StartStreaming(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    std::atomic<int>* chunk_count) {
    
    if (streaming_active_.load()) {
        AddDebugLog(L"[DEMUX] Streaming already active");
        return false;
    }
    
    log_callback_ = log_callback;
    chunk_count_ptr_ = chunk_count;
    cancel_token_ptr_ = &cancel_token;
    should_stop_ = false;
    
    LogMessage(L"Starting MPEG-TS demux streaming for " + channel_name_);
    
    // Start downloader thread
    downloader_thread_ = std::thread(&DemuxStreamManager::DownloadThreadFunction, this, playlist_url);
    
    // Start demux processing thread  
    demux_thread_ = std::thread(&DemuxStreamManager::DemuxThreadFunction, this);
    
    streaming_active_ = true;
    LogMessage(L"MPEG-TS demux streaming started");
    
    return true;
}

void DemuxStreamManager::StopStreaming() {
    if (!streaming_active_.load()) {
        return;
    }
    
    should_stop_ = true;
    streaming_active_ = false;
    
    LogMessage(L"Stopping MPEG-TS demux streaming");
    
    // Wait for threads to finish
    if (downloader_thread_.joinable()) {
        downloader_thread_.join();
    }
    if (demux_thread_.joinable()) {
        demux_thread_.join();
    }
    if (player_thread_.joinable()) {
        player_thread_.join();
    }
    
    // Stop demuxer
    if (demuxer_) {
        demuxer_->Stop();
    }
    
    // Terminate player process
    if (player_process_) {
        TerminateProcess(player_process_, 0);
        CloseHandle(player_process_);
        player_process_ = nullptr;
    }
    
    LogMessage(L"MPEG-TS demux streaming stopped");
}

std::wstring DemuxStreamManager::GenerateUniqueOutputDirectory() {
    wchar_t temp_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_path);
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::wostringstream dir_name;
    dir_name << temp_path << L"Tardsplaya_Demux_" << channel_name_ << L"_" << time_t;
    
    return dir_name.str();
}

bool DemuxStreamManager::CreateOutputDirectory() {
    try {
        std::filesystem::path dir_path(output_dir_);
        if (std::filesystem::create_directories(dir_path)) {
            AddDebugLog(L"[DEMUX] Created output directory: " + output_dir_);
            return true;
        } else if (std::filesystem::exists(dir_path)) {
            AddDebugLog(L"[DEMUX] Output directory already exists: " + output_dir_);
            return true;
        }
    } catch (const std::exception& e) {
        AddDebugLog(L"[DEMUX] Exception creating directory: " + Utf8ToWide(e.what()));
    }
    return false;
}

void DemuxStreamManager::DownloadThreadFunction(const std::wstring& playlist_url) {
    LogMessage(L"Download thread started");
    
    try {
        while (!should_stop_ && (!cancel_token_ptr_ || !cancel_token_ptr_->load())) {
            std::vector<std::wstring> segment_urls;
            if (DownloadPlaylistSegments(playlist_url, segment_urls)) {
                for (const auto& segment_url : segment_urls) {
                    if (should_stop_ || (cancel_token_ptr_ && cancel_token_ptr_->load())) {
                        break;
                    }
                    
                    std::vector<char> segment_data;
                    if (DownloadSegment(segment_url, segment_data)) {
                        // Pass data to demuxer
                        if (demuxer_ && demuxer_->IsActive()) {
                            demuxer_->ProcessTSData(segment_data.data(), segment_data.size());
                            UpdateChunkCount(static_cast<int>(segment_data.size()));
                        }
                    }
                    
                    // Small delay between segments
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            
            // Delay before next playlist refresh
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    } catch (const std::exception& e) {
        LogMessage(L"Download thread exception: " + Utf8ToWide(e.what()));
    }
    
    LogMessage(L"Download thread finished");
}

void DemuxStreamManager::DemuxThreadFunction() {
    LogMessage(L"Demux thread started");
    
    // Wait for some streams to be detected
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Launch media player when we have video and audio streams
    while (!should_stop_ && (!cancel_token_ptr_ || !cancel_token_ptr_->load())) {
        if (demuxer_) {
            auto stats = demuxer_->GetStats();
            if (stats.video_streams_count > 0 && stats.audio_streams_count > 0 && !player_process_) {
                LogMessage(L"Detected video and audio streams, launching media player");
                if (LaunchMediaPlayer()) {
                    player_thread_ = std::thread(&DemuxStreamManager::PlayerThreadFunction, this);
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    LogMessage(L"Demux thread finished");
}

void DemuxStreamManager::PlayerThreadFunction() {
    LogMessage(L"Player thread started");
    
    if (player_process_) {
        // Wait for player process to finish
        WaitForSingleObject(player_process_, INFINITE);
        CloseHandle(player_process_);
        player_process_ = nullptr;
    }
    
    LogMessage(L"Player thread finished");
}

bool DemuxStreamManager::LaunchMediaPlayer() {
    if (!demuxer_) {
        return false;
    }
    
    std::wstring video_file = demuxer_->GetVideoOutputFile();
    std::wstring audio_file = demuxer_->GetAudioOutputFile();
    
    if (video_file.empty() || audio_file.empty()) {
        LogMessage(L"Video or audio file not ready yet");
        return false;
    }
    
    std::wstring command_line = GetPlayerCommandLine();
    if (command_line.empty()) {
        LogMessage(L"Failed to generate player command line");
        return false;
    }
    
    LogMessage(L"Launching player: " + command_line);
    
    STARTUPINFOW si = { sizeof(si) };
    if (CreateProcessW(nullptr, const_cast<wchar_t*>(command_line.c_str()), 
                      nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &process_info_)) {
        player_process_ = process_info_.hProcess;
        LogMessage(L"Media player launched successfully");
        return true;
    } else {
        LogMessage(L"Failed to launch media player");
        return false;
    }
}

std::wstring DemuxStreamManager::GetPlayerCommandLine() {
    if (!demuxer_) {
        return L"";
    }
    
    std::wstring video_file = demuxer_->GetVideoOutputFile();
    std::wstring audio_file = demuxer_->GetAudioOutputFile();
    
    std::wstring player_type = MediaPlayerCommandBuilder::DetectPlayerType(player_path_);
    
    if (player_type == L"MPC") {
        return MediaPlayerCommandBuilder::BuildMPCCommand(player_path_, video_file, audio_file);
    } else if (player_type == L"VLC") {
        return MediaPlayerCommandBuilder::BuildVLCCommand(player_path_, video_file, audio_file);
    } else if (player_type == L"MPV") {
        return MediaPlayerCommandBuilder::BuildMPVCommand(player_path_, video_file, audio_file);
    } else {
        // Default to MPV-style command
        return MediaPlayerCommandBuilder::BuildMPVCommand(player_path_, video_file, audio_file);
    }
}

// Implement other helper functions...
bool DemuxStreamManager::DownloadPlaylistSegments(const std::wstring& playlist_url, std::vector<std::wstring>& segment_urls) {
    // Reuse existing playlist parsing logic
    std::string playlist_content;
    if (!HttpGetText(playlist_url, playlist_content, cancel_token_ptr_)) {
        return false;
    }
    
    // Simple M3U8 parsing - extract segment URLs
    segment_urls.clear();
    std::istringstream ss(playlist_content);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line[0] != '#') {
            std::wstring segment_url = Utf8ToWide(line);
            if (segment_url.find(L"http") != 0) {
                segment_url = JoinUrl(playlist_url, segment_url);
            }
            segment_urls.push_back(segment_url);
        }
    }
    
    return !segment_urls.empty();
}

bool DemuxStreamManager::DownloadSegment(const std::wstring& segment_url, std::vector<char>& segment_data) {
    return HttpGetBinary(segment_url, segment_data, cancel_token_ptr_);
}

void DemuxStreamManager::LogMessage(const std::wstring& message) {
    AddDebugLog(L"[DEMUX-STREAM] " + message);
    if (log_callback_) {
        log_callback_(message);
    }
}

void DemuxStreamManager::UpdateChunkCount(int count) {
    if (chunk_count_ptr_) {
        chunk_count_ptr_->store(count);
    }
}

MpegTSDemuxer::DemuxStats DemuxStreamManager::GetDemuxStats() const {
    if (demuxer_) {
        return demuxer_->GetStats();
    }
    return MpegTSDemuxer::DemuxStats{};
}

// MediaPlayerCommandBuilder Implementation
std::wstring MediaPlayerCommandBuilder::BuildMPCCommand(const std::wstring& player_path, 
                                                       const std::wstring& video_file, 
                                                       const std::wstring& audio_file) {
    std::wostringstream cmd;
    cmd << L"\"" << player_path << L"\" \"" << video_file << L"\" /dub \"" << audio_file << L"\"";
    return cmd.str();
}

std::wstring MediaPlayerCommandBuilder::BuildVLCCommand(const std::wstring& player_path, 
                                                       const std::wstring& video_file, 
                                                       const std::wstring& audio_file) {
    std::wostringstream cmd;
    cmd << L"\"" << player_path << L"\" \"" << video_file << L"\" --input-slave=\"" << audio_file << L"\"";
    return cmd.str();
}

std::wstring MediaPlayerCommandBuilder::BuildMPVCommand(const std::wstring& player_path, 
                                                       const std::wstring& video_file, 
                                                       const std::wstring& audio_file) {
    std::wostringstream cmd;
    cmd << L"\"" << player_path << L"\" \"" << video_file << L"\" --audio-file=\"" << audio_file << L"\"";
    return cmd.str();
}

std::wstring MediaPlayerCommandBuilder::DetectPlayerType(const std::wstring& player_path) {
    std::wstring player_name = player_path;
    std::transform(player_name.begin(), player_name.end(), player_name.begin(), ::towlower);
    
    if (player_name.find(L"mpc") != std::wstring::npos) {
        return L"MPC";
    } else if (player_name.find(L"vlc") != std::wstring::npos) {
        return L"VLC";  
    } else if (player_name.find(L"mpv") != std::wstring::npos) {
        return L"MPV";
    }
    
    return L"UNKNOWN";
}