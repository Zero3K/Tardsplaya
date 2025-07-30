#pragma once
#include <vector>
#include <memory>
#include <string>

// Forward declaration of TSReadEX classes
class CServiceFilter;

namespace tardsplaya {

/**
 * TSReadEX Integration for Tardsplaya
 * 
 * Provides optional MPEG-TS stream filtering and enhancement capabilities
 * using the TSReadEX library for improved stream compatibility.
 */
class TSReadEXProcessor {
public:
    struct Config {
        bool enabled = false;                    // Enable TSReadEX processing
        int program_number = -1;                 // Target program number (-1 for first program)
        bool remove_eit = true;                  // Remove EIT (program guide) packets
        bool stabilize_audio = true;             // Ensure consistent audio streams
        bool standardize_pids = true;           // Remap PIDs to standard values
        std::vector<int> exclude_pids;          // PIDs to exclude from output
        
        // Audio enhancement options
        bool ensure_stereo = false;             // Convert mono audio to stereo
        bool complement_missing_audio = true;   // Add silent audio if missing
        
        // Advanced options
        int timeout_seconds = 0;                // Processing timeout (0 = no timeout)
        int rate_limit_kbps = 0;               // Rate limiting (0 = unlimited)
    };

    TSReadEXProcessor();
    ~TSReadEXProcessor();

    /**
     * Initialize the processor with the given configuration
     */
    bool Initialize(const Config& config);

    /**
     * Process a chunk of MPEG-TS data
     * @param input Input TS packets
     * @param output Filtered/processed TS packets
     * @return true if processing succeeded
     */
    bool ProcessChunk(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

    /**
     * Check if TSReadEX processing is enabled and initialized
     */
    bool IsEnabled() const { return enabled_ && filter_ != nullptr; }

    /**
     * Get processing statistics
     */
    struct Stats {
        size_t packets_processed = 0;
        size_t packets_filtered = 0;
        size_t bytes_input = 0;
        size_t bytes_output = 0;
    };
    
    Stats GetStats() const { return stats_; }

    /**
     * Reset processor state (call between streams)
     */
    void Reset();

private:
    bool enabled_;
    Config config_;
    std::unique_ptr<CServiceFilter> filter_;
    Stats stats_;
    
    // PID filtering state
    std::vector<bool> pid_filter_table_;  // 8192 entries for all possible PIDs
    
    void SetupPIDFiltering();
    bool IsPacketFiltered(const uint8_t* packet) const;
};

} // namespace tardsplaya