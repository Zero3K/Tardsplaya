#include "tardsplaya_hls_reclock_integration.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <regex>
#include <process.h>
#include <windows.h>

// Handle filesystem include for different C++ standards
#ifdef _MSC_VER
    #if _MSC_VER >= 1914 && _MSVC_LANG >= 201703L
        #include <filesystem>
        namespace fs = std::filesystem;
    #else
        #include <experimental/filesystem>
        namespace fs = std::experimental::filesystem;
    #endif
#else
    #include <filesystem>
    namespace fs = std::filesystem;
#endif

namespace tardsplaya_hls_reclock {

    TardsplayaHLSReclock::TardsplayaHLSReclock(const TardsplayaReclockConfig& config)
        : config_(config) {
        FindReclockTool();
    }

    TardsplayaHLSReclock::~TardsplayaHLSReclock() {
        CleanupTempFiles();
    }

    ProcessingResult TardsplayaHLSReclock::ProcessHLSStream(const std::string& hls_url, 
                                                           const std::string& output_format,
                                                           ProgressCallback progress_cb) {
        ProcessingResult result;
        
        if (!IsReclockToolAvailable()) {
            result.error_message = "HLS PTS Reclock tool not found";
            return result;
        }

        if (progress_cb) {
            progress_cb(10, "Initializing HLS PTS correction...");
        }

        // Generate temporary output file
        std::string extension = (output_format == "flv") ? "flv" : "ts";
        result.output_path = GenerateTempFilename(extension);

        if (progress_cb) {
            progress_cb(30, "Starting HLS processing...");
        }

        // Execute the reclock tool
        bool success = ExecuteReclockTool(hls_url, result.output_path, output_format, progress_cb);
        
        if (success && fs::exists(result.output_path)) {
            result.success = true;
            if (progress_cb) {
                progress_cb(100, "HLS PTS correction completed");
            }
        } else {
            result.error_message = "Failed to process HLS stream with PTS correction";
            if (progress_cb) {
                progress_cb(100, "HLS PTS correction failed");
            }
        }

        return result;
    }

    bool TardsplayaHLSReclock::ProcessHLSStreamToPipe(const std::string& hls_url,
                                                    HANDLE write_pipe,
                                                    const std::string& output_format,
                                                    ProgressCallback progress_cb) {
        if (!IsReclockToolAvailable()) {
            return false;
        }

        if (progress_cb) {
            progress_cb(10, "Initializing HLS PTS correction with streaming output...");
        }

        // Build command line for stdout output
        std::ostringstream cmd;
        cmd << "\"" << reclock_tool_path_ << "\"";
        cmd << " --stdout";
        cmd << " -i \"" << hls_url << "\"";
        cmd << " -f " << output_format;
        
        if (config_.verbose_logging) {
            cmd << " --verbose";
        }
        
        cmd << " --threshold " << static_cast<int64_t>(config_.discontinuity_threshold_seconds * 1000000);
        cmd << " --delta-threshold " << config_.delta_threshold_seconds;

        if (config_.verbose_logging) {
            std::cout << "Executing streaming command: " << cmd.str() << std::endl;
        }

        if (progress_cb) {
            progress_cb(30, "Starting HLS processing with pipe output...");
        }

        // Set up process with stdout redirection
        STARTUPINFOA si = {};
        PROCESS_INFORMATION pi = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        std::string command_line = cmd.str();
        BOOL success = CreateProcessA(
            nullptr,
            const_cast<char*>(command_line.c_str()),
            nullptr,
            nullptr,
            TRUE, // Inherit handles
            0,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        if (!success) {
            if (config_.verbose_logging) {
                std::cerr << "Failed to start reclock tool process for streaming" << std::endl;
            }
            return false;
        }

        if (progress_cb) {
            progress_cb(50, "HLS PTS reclock tool streaming started...");
        }

        // Don't wait for process to complete in streaming mode - let it run
        // The calling code will manage the process lifetime
        CloseHandle(pi.hThread);
        // Keep process handle for caller to manage
        
        if (progress_cb) {
            progress_cb(90, "HLS PTS streaming process initiated");
        }

        return true;
    }

    bool TardsplayaHLSReclock::IsReclockToolAvailable() const {
        return !reclock_tool_path_.empty() && fs::exists(reclock_tool_path_);
    }

    std::string TardsplayaHLSReclock::GetReclockToolPath() const {
        return reclock_tool_path_;
    }

    void TardsplayaHLSReclock::CleanupTempFiles() {
        for (const auto& file : temp_files_) {
            try {
                if (fs::exists(file)) {
                    fs::remove(file);
                }
            } catch (const std::exception& e) {
                // Log error but continue cleanup
                if (config_.verbose_logging) {
                    std::cerr << "Failed to cleanup temp file " << file << ": " << e.what() << std::endl;
                }
            }
        }
        temp_files_.clear();
    }

    bool TardsplayaHLSReclock::FindReclockTool() {
        // Look for the reclock tool in the same directory as the executable
        char module_path[MAX_PATH];
        if (GetModuleFileNameA(nullptr, module_path, sizeof(module_path)) == 0) {
            return false;
        }

        fs::path exe_dir = fs::path(module_path).parent_path();
        
        // Check for both debug and release builds
        std::vector<std::string> possible_names = {
            "hls-pts-reclock.exe",
            "hls-pts-reclock_d.exe",  // Debug version
            "HLSPTSReclock.exe"
        };

        for (const auto& name : possible_names) {
            fs::path tool_path = exe_dir / name;
            if (fs::exists(tool_path)) {
                reclock_tool_path_ = tool_path.string();
                return true;
            }
        }

        // Also check relative paths
        std::vector<std::string> relative_paths = {
            "./hls-pts-reclock.exe",
            "../hls-pts-reclock.exe",
            "./Debug/hls-pts-reclock.exe",
            "./Release/hls-pts-reclock.exe"
        };

        for (const auto& rel_path : relative_paths) {
            if (fs::exists(rel_path)) {
                reclock_tool_path_ = fs::absolute(rel_path).string();
                return true;
            }
        }

        return false;
    }

    std::string TardsplayaHLSReclock::GenerateTempFilename(const std::string& extension) {
        static int counter = 0;
        std::ostringstream oss;
        
        fs::path temp_dir = config_.temp_directory;
        
        // Create temp directory if it doesn't exist
        try {
            fs::create_directories(temp_dir);
        } catch (const std::exception&) {
            // Fallback to current directory
            temp_dir = ".";
        }

        oss << temp_dir.string() << "/hls_reclock_" << GetCurrentProcessId() << "_" << (++counter) << "." << extension;
        
        std::string filename = oss.str();
        temp_files_.push_back(filename);
        return filename;
    }

    bool TardsplayaHLSReclock::ExecuteReclockTool(const std::string& input_url, 
                                                 const std::string& output_path,
                                                 const std::string& format,
                                                 ProgressCallback progress_cb) {
        // Build command line
        std::ostringstream cmd;
        cmd << "\"" << reclock_tool_path_ << "\"";
        cmd << " -i \"" << input_url << "\"";
        cmd << " -o \"" << output_path << "\"";
        cmd << " -f " << format;
        
        if (config_.verbose_logging) {
            cmd << " --verbose";
        }
        
        cmd << " --threshold " << static_cast<int64_t>(config_.discontinuity_threshold_seconds * 1000000);
        cmd << " --delta-threshold " << config_.delta_threshold_seconds;

        if (config_.verbose_logging) {
            std::cout << "Executing: " << cmd.str() << std::endl;
        }

        if (progress_cb) {
            progress_cb(50, "Executing HLS PTS reclock tool...");
        }

        // Execute the command
        STARTUPINFOA si = {};
        PROCESS_INFORMATION pi = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE; // Hide console window

        std::string command_line = cmd.str();
        BOOL success = CreateProcessA(
            nullptr,
            const_cast<char*>(command_line.c_str()),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        if (!success) {
            if (config_.verbose_logging) {
                std::cerr << "Failed to start reclock tool process" << std::endl;
            }
            return false;
        }

        // Wait for process to complete
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code;
        GetExitCodeProcess(pi.hProcess, &exit_code);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (progress_cb) {
            progress_cb(90, "HLS PTS reclock tool completed");
        }

        return exit_code == 0;
    }

    ProcessingResult TardsplayaHLSReclock::ParseReclockOutput(const std::string& output) {
        ProcessingResult result;
        
        // Parse statistics from tool output (simplified implementation)
        std::regex stats_regex(R"(Total packets processed: (\d+))");
        std::regex discontinuities_regex(R"(Discontinuities detected: (\d+))");
        std::regex corrections_regex(R"(Timestamp corrections applied: (\d+))");
        
        std::smatch match;
        if (std::regex_search(output, match, stats_regex)) {
            result.total_segments_processed = std::stoi(match[1]);
        }
        
        if (std::regex_search(output, match, discontinuities_regex)) {
            result.discontinuities_detected = std::stoi(match[1]);
        }
        
        if (std::regex_search(output, match, corrections_regex)) {
            result.timestamp_corrections = std::stoi(match[1]);
        }
        
        return result;
    }

    // Utility functions implementation
    namespace integration_utils {
        
        bool ShouldApplyPTSCorrection(const std::string& url) {
            // Apply PTS correction for:
            // 1. Live streams (URLs containing "live", "stream", etc.)
            // 2. HLS streams (.m3u8 files)
            // 3. Known problematic sources
            
            std::string lower_url = url;
            std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), ::tolower);
            
            // Check for HLS streams
            if (lower_url.find(".m3u8") != std::string::npos) {
                return true;
            }
            
            // Check for live stream indicators
            std::vector<std::string> live_indicators = {
                "live", "stream", "twitch", "youtube", "rtmp", "real", "broadcast"
            };
            
            for (const auto& indicator : live_indicators) {
                if (lower_url.find(indicator) != std::string::npos) {
                    return true;
                }
            }
            
            return false;
        }
        
        std::string ConvertStreamFormat(const std::string& tardsplaya_format) {
            // Convert Tardsplaya internal formats to reclock tool formats
            if (tardsplaya_format == "ts" || tardsplaya_format == "mpegts" || tardsplaya_format == "transport") {
                return "mpegts";
            } else if (tardsplaya_format == "flv" || tardsplaya_format == "rtmp") {
                return "flv";
            }
            
            // Default to MPEG-TS
            return "mpegts";
        }
        
        TardsplayaReclockConfig CreateConfigForStreamType(const std::string& stream_type) {
            TardsplayaReclockConfig config;
            
            if (stream_type == "live") {
                // More aggressive settings for live streams
                config.discontinuity_threshold_seconds = 0.5;
                config.delta_threshold_seconds = 5.0;
                config.verbose_logging = false;
            } else if (stream_type == "vod") {
                // More conservative settings for VOD
                config.discontinuity_threshold_seconds = 2.0;
                config.delta_threshold_seconds = 30.0;
                config.verbose_logging = false;
            } else {
                // Default settings
                config.discontinuity_threshold_seconds = 1.0;
                config.delta_threshold_seconds = 10.0;
                config.verbose_logging = false;
            }
            
            return config;
        }
    }

} // namespace tardsplaya_hls_reclock