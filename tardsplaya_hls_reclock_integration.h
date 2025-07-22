#pragma once

// Integration wrapper for HLS PTS Discontinuity Reclock functionality in Tardsplaya
// This provides an interface for Tardsplaya to use the standalone HLS reclock tool

#include <string>
#include <memory>
#include <functional>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tardsplaya_hls_reclock {

    // Configuration for HLS PTS correction in Tardsplaya
    struct TardsplayaReclockConfig {
        bool enable_pts_correction = true;
        bool verbose_logging = false;
        double discontinuity_threshold_seconds = 1.0;
        double delta_threshold_seconds = 10.0;
        std::string temp_directory = "temp";
        
        TardsplayaReclockConfig() = default;
    };

    // Result information from HLS processing
    struct ProcessingResult {
        bool success = false;
        std::string output_path;
        std::string error_message;
        
        // Statistics
        int total_segments_processed = 0;
        int discontinuities_detected = 0;
        int timestamp_corrections = 0;
        
        ProcessingResult() = default;
    };

    // Callback for progress updates
    using ProgressCallback = std::function<void(int progress_percent, const std::string& status)>;

    // Main integration class for Tardsplaya
    class TardsplayaHLSReclock {
    public:
        TardsplayaHLSReclock(const TardsplayaReclockConfig& config = TardsplayaReclockConfig());
        ~TardsplayaHLSReclock();

        // Process HLS URL and return path to corrected stream
        ProcessingResult ProcessHLSStream(const std::string& hls_url, 
                                        const std::string& output_format = "mpegts",
                                        ProgressCallback progress_cb = nullptr);

        // Process HLS stream and pipe directly to a process (streaming approach)
        bool ProcessHLSStreamToPipe(const std::string& hls_url,
                                   HANDLE write_pipe,
                                   const std::string& output_format = "mpegts", 
                                   ProgressCallback progress_cb = nullptr);

        // Check if the reclock tool is available
        bool IsReclockToolAvailable() const;

        // Get the path to the reclock executable
        std::string GetReclockToolPath() const;

        // Update configuration
        void SetConfig(const TardsplayaReclockConfig& config) { config_ = config; }
        const TardsplayaReclockConfig& GetConfig() const { return config_; }

        // Clean up temporary files
        void CleanupTempFiles();

    private:
        TardsplayaReclockConfig config_;
        std::string reclock_tool_path_;
        std::vector<std::string> temp_files_;

        // Internal helper methods
        bool FindReclockTool();
        std::string GenerateTempFilename(const std::string& extension);
        bool ExecuteReclockTool(const std::string& input_url, 
                              const std::string& output_path,
                              const std::string& format,
                              ProgressCallback progress_cb);
        ProcessingResult ParseReclockOutput(const std::string& output);
    };

    // Utility functions for Tardsplaya integration
    namespace integration_utils {
        // Check if URL is likely to have PTS discontinuities (e.g., live streams)
        bool ShouldApplyPTSCorrection(const std::string& url);
        
        // Convert Tardsplaya stream format to reclock format
        std::string ConvertStreamFormat(const std::string& tardsplaya_format);
        
        // Create recommended configuration based on stream type
        TardsplayaReclockConfig CreateConfigForStreamType(const std::string& stream_type);
    }

} // namespace tardsplaya_hls_reclock