#pragma once
// TSReadEX Integration for Tardsplaya Transport Stream Router
// Provides enhanced MPEG-TS processing capabilities through TSReadEX integration

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>
#include <chrono>
#include <thread>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Forward declarations
namespace tsduck_transport {
    class TransportStreamRouter;
    struct TSPacket;
    struct BufferStats;
}

namespace tsduck_transport {

    // TSReadEX configuration options
    struct TSReadEXConfig {
        bool enabled = false;                           // Enable TSReadEX processing
        
        // Basic filtering options
        std::vector<int> exclude_pids;                  // PIDs to exclude (-x option)
        int program_selection = 0;                      // Program number/index (-n option, 0=disabled)
        int rate_limit_kbps = 0;                       // Rate limiting in KB/s (-l option, 0=unlimited)
        int timeout_seconds = 0;                        // Timeout in seconds (-t option, 0=no timeout)
        
        // Audio processing options
        int audio1_mode = 0;                           // First audio processing (-a option)
                                                       // 0=passthrough, 1=ensure exists, +4=mono to stereo, +8=dual-mono split
        int audio2_mode = 0;                           // Second audio processing (-b option)
                                                       // 0=passthrough, 1=ensure exists, 2=remove, 3=copy from first, +4=mono to stereo
        
        // Caption/subtitle options
        int caption_mode = 0;                          // Caption processing (-c option)
                                                       // 0=passthrough, 1=ensure exists, 2=remove, +4=insert dummy data
        int superimpose_mode = 0;                      // Superimpose processing (-u option)
                                                       // 0=passthrough, 1=ensure exists, 2=remove, +4=insert dummy data
        
        // Advanced options
        bool enable_arib_conversion = false;            // Convert ARIB captions to ID3 timed-metadata (-d option)
        bool enable_pts_monotonic = false;             // Ensure monotonic PTS for ffmpeg compatibility
        bool enable_ffmpeg_bug_workaround = false;     // Add workaround data for ffmpeg bugs
        
        // Debug options
        std::wstring trace_file;                        // Trace output file (-r option, empty=disabled)
        bool trace_to_stdout = false;                   // Output trace to stdout instead of TS data
        
        // Processing mode
        enum class ProcessingMode {
            NORMAL = 0,         // Normal file/stream processing
            PREALLOCATED = 1,   // Preallocated file processing
            NONBLOCKING = 2     // Non-blocking pipe processing
        };
        ProcessingMode mode = ProcessingMode::NORMAL;
        
        // Quality of Service
        int buffer_size_kb = 64;                       // Internal buffer size in KB
        bool low_latency_mode = false;                 // Enable low-latency optimizations
        
        // Validation
        bool IsValid() const;
        std::wstring GetCommandLine() const;           // Generate command line for external process
    };

    // TSReadEX processor - handles integration with TSReadEX functionality
    class TSReadEXProcessor {
    public:
        TSReadEXProcessor();
        ~TSReadEXProcessor();
        
        // Configuration
        void SetConfig(const TSReadEXConfig& config) { config_ = config; }
        const TSReadEXConfig& GetConfig() const { return config_; }
        
        // Processing methods
        enum class IntegrationMode {
            DISABLED,           // TSReadEX integration disabled
            EXTERNAL_PROCESS,   // Use external TSReadEX executable
            INTERNAL_LIBRARY,   // Use integrated TSReadEX code (future)
            AUTO_DETECT        // Automatically choose best available method
        };
        
        // Initialize processor with specified integration mode
        bool Initialize(IntegrationMode mode = IntegrationMode::AUTO_DETECT);
        
        // Process transport stream data
        // Returns processed data, or original data if processing fails/disabled
        std::vector<uint8_t> ProcessTSData(const std::vector<uint8_t>& input_data, 
                                          std::atomic<bool>& cancel_token,
                                          std::function<void(const std::wstring&)> log_callback = nullptr);
        
        // Stream processing (for continuous data)
        bool StartStreamProcessing(std::atomic<bool>& cancel_token,
                                 std::function<void(const std::wstring&)> log_callback = nullptr);
        void StopStreamProcessing();
        bool WriteStreamData(const std::vector<uint8_t>& data);
        std::vector<uint8_t> ReadProcessedData(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
        
        // Status and statistics
        bool IsAvailable() const { return mode_ != IntegrationMode::DISABLED; }
        bool IsProcessing() const { return processing_active_; }
        IntegrationMode GetActiveMode() const { return mode_; }
        
        struct ProcessingStats {
            size_t bytes_processed = 0;
            size_t packets_processed = 0;
            size_t packets_filtered = 0;
            std::chrono::milliseconds processing_time{0};
            std::chrono::steady_clock::time_point last_activity;
            bool has_errors = false;
            std::wstring last_error;
        };
        ProcessingStats GetStats() const;
        
        // Static utility methods
        static bool IsTSReadEXAvailable();                          // Check if TSReadEX executable exists
        static std::wstring GetTSReadEXPath();                      // Get path to TSReadEX executable
        static std::vector<std::wstring> GetSupportedFeatures();    // Get list of supported features
        static std::wstring GetVersion();                           // Get TSReadEX version if available
        
        // Error handling
        std::wstring GetLastError() const { return last_error_; }
        void ClearError() { last_error_.clear(); }
        
    private:
        TSReadEXConfig config_;
        IntegrationMode mode_ = IntegrationMode::DISABLED;
        std::atomic<bool> processing_active_{false};
        std::wstring last_error_;
        
        // External process handling
        struct ExternalProcess {
            HANDLE process_handle = INVALID_HANDLE_VALUE;
            HANDLE stdin_handle = INVALID_HANDLE_VALUE;
            HANDLE stdout_handle = INVALID_HANDLE_VALUE;
            HANDLE stderr_handle = INVALID_HANDLE_VALUE;
            std::thread reader_thread;
            std::atomic<bool> active{false};
        };
        std::unique_ptr<ExternalProcess> external_process_;
        
        // Statistics
        mutable std::mutex stats_mutex_;
        ProcessingStats stats_;
        
        // Internal methods
        bool InitializeExternalProcess();
        bool InitializeInternalLibrary();
        void CleanupExternalProcess();
        
        // External process management
        bool LaunchTSReadEXProcess(const std::wstring& input_file, const std::wstring& output_file);
        bool LaunchTSReadEXStreamProcess();
        void ExternalProcessReaderThread(std::atomic<bool>& cancel_token);
        
        // Data processing
        std::vector<uint8_t> ProcessWithExternalProcess(const std::vector<uint8_t>& input_data,
                                                        std::atomic<bool>& cancel_token);
        std::vector<uint8_t> ProcessWithInternalLibrary(const std::vector<uint8_t>& input_data,
                                                        std::atomic<bool>& cancel_token);
        
        // Utility methods
        std::wstring CreateTempFileName(const std::wstring& suffix = L".ts");
        bool ValidateProcessedData(const std::vector<uint8_t>& data);
        void UpdateStats(size_t bytes_in, size_t bytes_out, std::chrono::milliseconds processing_time);
        void LogMessage(const std::wstring& message, std::function<void(const std::wstring&)> log_callback);
    };

    // Enhanced transport stream router with TSReadEX integration
    // Note: This would inherit from TransportStreamRouter in the actual implementation
    class EnhancedTransportStreamRouter {
    public:
        EnhancedTransportStreamRouter();
        ~EnhancedTransportStreamRouter();
        
        // TSReadEX configuration
        void SetTSReadEXConfig(const TSReadEXConfig& config);
        const TSReadEXConfig& GetTSReadEXConfig() const;
        bool IsTSReadEXEnabled() const;
        
        // Enhanced routing with TSReadEX processing
        bool StartEnhancedRouting(const std::wstring& hls_playlist_url,
                                const void* router_config,  // RouterConfig forward declaration
                                const TSReadEXConfig& tsreadex_config,
                                std::atomic<bool>& cancel_token,
                                std::function<void(const std::wstring&)> log_callback = nullptr);
        
        // Get combined statistics
        struct EnhancedBufferStats {
            // Base BufferStats fields (to be expanded when including full header)
            size_t buffered_packets = 0;
            size_t total_packets_processed = 0;
            double buffer_utilization = 0.0;
            
            TSReadEXProcessor::ProcessingStats tsreadex_stats;
            bool tsreadex_active = false;
            std::wstring processing_pipeline;  // Description of active processing pipeline
        };
        EnhancedBufferStats GetEnhancedBufferStats() const;
        
    private:
        std::unique_ptr<TSReadEXProcessor> tsreadex_processor_;
        TSReadEXConfig tsreadex_config_;
        
        // Enhanced processing pipeline
        std::vector<uint8_t> ProcessSegmentWithTSReadEX(const std::vector<uint8_t>& segment_data,
                                                        std::atomic<bool>& cancel_token);
    };

} // namespace tsduck_transport