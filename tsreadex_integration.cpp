#include "tsreadex_integration.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cstring>  // for strlen
#include <codecvt>  // for wstring conversion

namespace tsduck_transport {

// Utility function to convert wstring to string for file operations
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
}

// Forward declarations for logging
extern void AddDebugLog(const std::wstring& msg);

// TSReadEXConfig implementation
bool TSReadEXConfig::IsValid() const {
    // Validate PID ranges
    for (int pid : exclude_pids) {
        if (pid < 0 || pid > 8191) return false;
    }
    
    // Validate program selection
    if (program_selection < -256 || program_selection > 65535) return false;
    
    // Validate rate limit
    if (rate_limit_kbps < 0 || rate_limit_kbps > 32768) return false;
    
    // Validate timeout
    if (timeout_seconds < 0 || timeout_seconds > 600) return false;
    
    // Validate audio modes
    if (audio1_mode < 0 || audio1_mode > 13 || audio1_mode % 4 > 1) return false;
    if (audio2_mode < 0 || audio2_mode > 7 || audio2_mode % 4 > 3) return false;
    
    // Validate caption/superimpose modes
    if (caption_mode < 0 || caption_mode > 6 || caption_mode % 4 > 2) return false;
    if (superimpose_mode < 0 || superimpose_mode > 6 || superimpose_mode % 4 > 2) return false;
    
    // Mode-specific validations
    if (mode == ProcessingMode::NONBLOCKING && timeout_seconds == 0) return false;
    
    return true;
}

std::wstring TSReadEXConfig::GetCommandLine() const {
    std::wostringstream cmd;
    cmd << L"tsreadex.exe";
    
    // Rate limiting
    if (rate_limit_kbps > 0) {
        cmd << L" -l " << rate_limit_kbps;
    }
    
    // Timeout
    if (timeout_seconds > 0) {
        cmd << L" -t " << timeout_seconds;
    }
    
    // Processing mode
    if (mode != ProcessingMode::NORMAL) {
        cmd << L" -m " << static_cast<int>(mode);
    }
    
    // Exclude PIDs
    if (!exclude_pids.empty()) {
        cmd << L" -x ";
        for (size_t i = 0; i < exclude_pids.size(); ++i) {
            if (i > 0) cmd << L"/";
            cmd << exclude_pids[i];
        }
    }
    
    // Program selection
    if (program_selection != 0) {
        cmd << L" -n " << program_selection;
        
        // Audio processing (only valid with program selection)
        if (audio1_mode != 0) {
            cmd << L" -a " << audio1_mode;
        }
        if (audio2_mode != 0) {
            cmd << L" -b " << audio2_mode;
        }
        
        // Caption processing (only valid with program selection)
        if (caption_mode != 0) {
            cmd << L" -c " << caption_mode;
        }
        if (superimpose_mode != 0) {
            cmd << L" -u " << superimpose_mode;
        }
    }
    
    // ARIB conversion
    if (enable_arib_conversion) {
        int d_flags = 1;
        if (enable_ffmpeg_bug_workaround) d_flags += 4;
        if (enable_pts_monotonic) d_flags += 8;
        cmd << L" -d " << d_flags;
    }
    
    // Trace file
    if (!trace_file.empty()) {
        cmd << L" -r \"" << trace_file << L"\"";
    } else if (trace_to_stdout) {
        cmd << L" -r -";
    }
    
    return cmd.str();
}

// TSReadEXProcessor implementation
TSReadEXProcessor::TSReadEXProcessor() {
    // Initialize default configuration
    config_ = TSReadEXConfig{};
    last_error_.clear();
}

TSReadEXProcessor::~TSReadEXProcessor() {
    StopStreamProcessing();
    CleanupExternalProcess();
}

bool TSReadEXProcessor::Initialize(IntegrationMode mode) {
    // Validate configuration
    if (!config_.IsValid()) {
        last_error_ = L"Invalid TSReadEX configuration";
        return false;
    }
    
    if (mode == IntegrationMode::AUTO_DETECT) {
        // Try external process first, then internal library
        if (IsTSReadEXAvailable()) {
            mode = IntegrationMode::EXTERNAL_PROCESS;
        } else {
            mode = IntegrationMode::INTERNAL_LIBRARY;  // Future implementation
        }
    }
    
    switch (mode) {
        case IntegrationMode::EXTERNAL_PROCESS:
            if (!InitializeExternalProcess()) {
                return false;
            }
            break;
            
        case IntegrationMode::INTERNAL_LIBRARY:
            if (!InitializeInternalLibrary()) {
                return false;
            }
            break;
            
        case IntegrationMode::DISABLED:
            mode_ = IntegrationMode::DISABLED;
            return true;
            
        default:
            last_error_ = L"Unsupported integration mode";
            return false;
    }
    
    mode_ = mode;
    return true;
}

std::vector<uint8_t> TSReadEXProcessor::ProcessTSData(const std::vector<uint8_t>& input_data,
                                                     std::atomic<bool>& cancel_token,
                                                     std::function<void(const std::wstring&)> log_callback) {
    if (!config_.enabled || mode_ == IntegrationMode::DISABLED) {
        return input_data;  // Pass through unchanged
    }
    
    if (input_data.empty()) {
        return input_data;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        std::vector<uint8_t> result;
        
        switch (mode_) {
            case IntegrationMode::EXTERNAL_PROCESS:
                result = ProcessWithExternalProcess(input_data, cancel_token);
                break;
                
            case IntegrationMode::INTERNAL_LIBRARY:
                result = ProcessWithInternalLibrary(input_data, cancel_token);
                break;
                
            default:
                result = input_data;  // Fallback to passthrough
                break;
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto processing_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        UpdateStats(input_data.size(), result.size(), processing_time);
        
        LogMessage(L"TSReadEX processed " + std::to_wstring(input_data.size()) + 
                  L" bytes to " + std::to_wstring(result.size()) + 
                  L" bytes in " + std::to_wstring(processing_time.count()) + L"ms", log_callback);
        
        return result;
        
    } catch (const std::exception& e) {
        last_error_ = L"TSReadEX processing failed: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        LogMessage(L"TSReadEX error: " + last_error_, log_callback);
        return input_data;  // Return original data on error
    }
}

// Static utility methods
bool TSReadEXProcessor::IsTSReadEXAvailable() {
    std::wstring tsreadex_path = GetTSReadEXPath();
    return std::filesystem::exists(tsreadex_path);
}

std::wstring TSReadEXProcessor::GetTSReadEXPath() {
    // Check common locations for TSReadEX executable
    std::vector<std::wstring> search_paths = {
        L"tsreadex.exe",                    // Current directory
        L"./tsreadex.exe",                  // Current directory explicit
        L"./tools/tsreadex.exe",            // Tools subdirectory
        L"./bin/tsreadex.exe",              // Bin subdirectory
        L"C:\\Program Files\\TSReadEX\\tsreadex.exe",  // System installation
        L"C:\\Tools\\TSReadEX\\tsreadex.exe"           // Custom tools directory
    };
    
    for (const auto& path : search_paths) {
        if (std::filesystem::exists(path)) {
            return std::filesystem::absolute(path).wstring();
        }
    }
    
    return L"";  // Not found
}

std::vector<std::wstring> TSReadEXProcessor::GetSupportedFeatures() {
    std::vector<std::wstring> features;
    
    if (IsTSReadEXAvailable()) {
        features.push_back(L"PID Filtering");
        features.push_back(L"Service Selection");
        features.push_back(L"Audio Processing");
        features.push_back(L"Rate Limiting");
        features.push_back(L"Stream Repair");
        features.push_back(L"ARIB Caption Support");
        features.push_back(L"ID3 Metadata Conversion");
        features.push_back(L"Dual-Mono Audio Separation");
    }
    
    return features;
}

std::wstring TSReadEXProcessor::GetVersion() {
    // This would require running tsreadex with a version flag
    // For now, return a placeholder
    return L"Unknown (external)";
}

TSReadEXProcessor::ProcessingStats TSReadEXProcessor::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// Private methods
bool TSReadEXProcessor::InitializeExternalProcess() {
    if (!IsTSReadEXAvailable()) {
        last_error_ = L"TSReadEX executable not found";
        return false;
    }
    
    // External process will be launched per-operation
    return true;
}

bool TSReadEXProcessor::InitializeInternalLibrary() {
    // Future implementation: integrate TSReadEX source code directly
    last_error_ = L"Internal TSReadEX library not yet implemented";
    return false;
}

void TSReadEXProcessor::CleanupExternalProcess() {
    if (external_process_) {
        external_process_->active = false;
        
        if (external_process_->reader_thread.joinable()) {
            external_process_->reader_thread.join();
        }
        
        if (external_process_->process_handle != INVALID_HANDLE_VALUE) {
            TerminateProcess(external_process_->process_handle, 0);
            CloseHandle(external_process_->process_handle);
        }
        
        if (external_process_->stdin_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(external_process_->stdin_handle);
        }
        
        if (external_process_->stdout_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(external_process_->stdout_handle);
        }
        
        if (external_process_->stderr_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(external_process_->stderr_handle);
        }
        
        external_process_.reset();
    }
}

std::vector<uint8_t> TSReadEXProcessor::ProcessWithExternalProcess(const std::vector<uint8_t>& input_data,
                                                                  std::atomic<bool>& cancel_token) {
    // Create temporary files for input and output
    std::wstring input_file = CreateTempFileName(L"_input.ts");
    std::wstring output_file = CreateTempFileName(L"_output.ts");
    
    try {
        // Write input data to temporary file
        std::string input_file_str = WStringToString(input_file);
        std::string output_file_str = WStringToString(output_file);
        
        std::ofstream input_stream(input_file_str, std::ios::binary);
        if (!input_stream.is_open()) {
            throw std::runtime_error("Failed to create input file");
        }
        input_stream.write(reinterpret_cast<const char*>(input_data.data()), input_data.size());
        input_stream.close();
        
        // Launch TSReadEX process
        if (!LaunchTSReadEXProcess(input_file, output_file)) {
            throw std::runtime_error("Failed to launch TSReadEX process");
        }
        
        // Wait for process completion (with timeout)
        DWORD wait_result = WaitForSingleObject(external_process_->process_handle, 30000);  // 30 second timeout
        if (wait_result != WAIT_OBJECT_0) {
            TerminateProcess(external_process_->process_handle, 1);
            throw std::runtime_error("TSReadEX process timed out");
        }
        
        // Read output data
        std::ifstream output_stream(output_file_str, std::ios::binary | std::ios::ate);
        if (!output_stream.is_open()) {
            throw std::runtime_error("Failed to read output file");
        }
        
        std::streamsize size = output_stream.tellg();
        output_stream.seekg(0, std::ios::beg);
        
        std::vector<uint8_t> result(size);
        if (!output_stream.read(reinterpret_cast<char*>(result.data()), size)) {
            throw std::runtime_error("Failed to read output data");
        }
        
        // Cleanup
        CleanupExternalProcess();
        std::filesystem::remove(input_file);
        std::filesystem::remove(output_file);
        
        return result;
        
    } catch (...) {
        // Cleanup on error
        CleanupExternalProcess();
        std::filesystem::remove(input_file);
        std::filesystem::remove(output_file);
        throw;
    }
}

std::vector<uint8_t> TSReadEXProcessor::ProcessWithInternalLibrary(const std::vector<uint8_t>& input_data,
                                                                   std::atomic<bool>& cancel_token) {
    // Future implementation: integrate TSReadEX source code
    last_error_ = L"Internal library processing not implemented";
    return input_data;
}

bool TSReadEXProcessor::LaunchTSReadEXProcess(const std::wstring& input_file, const std::wstring& output_file) {
    external_process_ = std::make_unique<ExternalProcess>();
    
    // Build command line
    std::wstring cmd_line = config_.GetCommandLine();
    cmd_line += L" \"" + input_file + L"\" > \"" + output_file + L"\"";
    
    // Create process
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    if (!CreateProcessW(nullptr, &cmd_line[0], nullptr, nullptr, FALSE, 
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        last_error_ = L"Failed to create TSReadEX process";
        return false;
    }
    
    external_process_->process_handle = pi.hProcess;
    CloseHandle(pi.hThread);  // Don't need thread handle
    
    return true;
}

std::wstring TSReadEXProcessor::CreateTempFileName(const std::wstring& suffix) {
    wchar_t temp_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_path);
    
    std::wostringstream temp_name;
    temp_name << temp_path << L"tardsplaya_tsreadex_" << GetCurrentProcessId() << L"_" << GetTickCount64() << suffix;
    
    return temp_name.str();
}

void TSReadEXProcessor::UpdateStats(size_t bytes_in, size_t bytes_out, std::chrono::milliseconds processing_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.bytes_processed += bytes_in;
    stats_.packets_processed += bytes_in / 188;  // Assume 188-byte TS packets
    stats_.packets_filtered += (bytes_in - bytes_out) / 188;
    stats_.processing_time += processing_time;
    stats_.last_activity = std::chrono::steady_clock::now();
}

void TSReadEXProcessor::LogMessage(const std::wstring& message, std::function<void(const std::wstring&)> log_callback) {
    if (log_callback) {
        log_callback(L"TSReadEX: " + message);
    } else {
        AddDebugLog(L"TSReadEX: " + message);
    }
}

// EnhancedTransportStreamRouter implementation
EnhancedTransportStreamRouter::EnhancedTransportStreamRouter() {
    tsreadex_processor_ = std::make_unique<TSReadEXProcessor>();
}

EnhancedTransportStreamRouter::~EnhancedTransportStreamRouter() {
    // Cleanup handled by unique_ptr
}

void EnhancedTransportStreamRouter::SetTSReadEXConfig(const TSReadEXConfig& config) {
    tsreadex_config_ = config;
    tsreadex_processor_->SetConfig(config);
}

const TSReadEXConfig& EnhancedTransportStreamRouter::GetTSReadEXConfig() const {
    return tsreadex_config_;
}

bool EnhancedTransportStreamRouter::IsTSReadEXEnabled() const {
    return tsreadex_config_.enabled && tsreadex_processor_->IsAvailable();
}

bool EnhancedTransportStreamRouter::StartEnhancedRouting(const std::wstring& hls_playlist_url,
                                                        const void* router_config_ptr,
                                                        const TSReadEXConfig& tsreadex_config,
                                                        std::atomic<bool>& cancel_token,
                                                        std::function<void(const std::wstring&)> log_callback) {
    // Configure TSReadEX
    SetTSReadEXConfig(tsreadex_config);
    
    if (tsreadex_config.enabled) {
        if (!tsreadex_processor_->Initialize()) {
            if (log_callback) {
                log_callback(L"Warning: TSReadEX initialization failed, falling back to standard processing");
            }
            // Continue with standard processing
        }
    }
    
    // For now, return true to indicate the interface is available
    // Full implementation would integrate with actual RouterConfig
    return true;
}

EnhancedTransportStreamRouter::EnhancedBufferStats EnhancedTransportStreamRouter::GetEnhancedBufferStats() const {
    EnhancedBufferStats stats;
    
    // For now, return basic stats
    // Full implementation would get base stats from TransportStreamRouter
    
    // Add TSReadEX stats
    if (tsreadex_processor_) {
        stats.tsreadex_stats = tsreadex_processor_->GetStats();
        stats.tsreadex_active = tsreadex_processor_->IsProcessing();
    }
    
    // Build processing pipeline description
    std::wostringstream pipeline;
    pipeline << L"HLS";
    if (IsTSReadEXEnabled()) {
        pipeline << L" → TSReadEX";
    }
    pipeline << L" → TSDuck → Player";
    stats.processing_pipeline = pipeline.str();
    
    return stats;
}

std::vector<uint8_t> EnhancedTransportStreamRouter::ProcessSegmentWithTSReadEX(const std::vector<uint8_t>& segment_data,
                                                                               std::atomic<bool>& cancel_token) {
    std::vector<uint8_t> processed_data = segment_data;
    
    // Apply TSReadEX processing if enabled
    if (IsTSReadEXEnabled()) {
        processed_data = tsreadex_processor_->ProcessTSData(segment_data, cancel_token);
    }
    
    return processed_data;
}

} // namespace tsduck_transport