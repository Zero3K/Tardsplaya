// Example: Using TSReadEX Integration with Tardsplaya
// This example demonstrates how to configure and use TSReadEX for enhanced transport stream processing

#include "tsreadex_integration.h"
#include <iostream>

void ExampleBasicUsage() {
    std::wcout << L"=== Basic TSReadEX Usage Example ===" << std::endl;
    
    // Create basic configuration
    tsduck_transport::TSReadEXConfig config;
    config.enabled = true;
    config.exclude_pids = {0x12, 0x26, 0x27};  // Remove EIT and other metadata
    config.program_selection = -1;              // Select first program in PAT
    config.audio1_mode = 1;                     // Ensure first audio exists
    config.audio2_mode = 3;                     // Copy first audio if second doesn't exist
    
    std::wcout << L"Command line: " << config.GetCommandLine() << std::endl;
    
    // Create processor
    tsduck_transport::TSReadEXProcessor processor;
    processor.SetConfig(config);
    
    if (processor.Initialize()) {
        std::wcout << L"TSReadEX processor initialized successfully" << std::endl;
    } else {
        std::wcout << L"TSReadEX processor initialization failed: " << processor.GetLastError() << std::endl;
    }
}

void ExampleAdvancedConfiguration() {
    std::wcout << L"\n=== Advanced TSReadEX Configuration Example ===" << std::endl;
    
    // Advanced configuration for Japanese digital TV
    tsduck_transport::TSReadEXConfig config;
    config.enabled = true;
    config.program_selection = 100;             // Specific service ID
    config.audio1_mode = 9;                     // Dual-mono separation + ensure exists
    config.audio2_mode = 5;                     // Mono to stereo + ensure exists
    config.caption_mode = 5;                    // Caption + dummy data insertion
    config.enable_arib_conversion = true;       // Convert ARIB to ID3
    config.enable_ffmpeg_bug_workaround = true; // Enable ffmpeg compatibility
    config.enable_pts_monotonic = true;        // Ensure monotonic PTS
    config.rate_limit_kbps = 8000;             // 8MB/s rate limit
    config.timeout_seconds = 30;                // 30 second timeout
    
    std::wcout << L"Advanced command line: " << config.GetCommandLine() << std::endl;
    
    // Features check
    auto features = tsduck_transport::TSReadEXProcessor::GetSupportedFeatures();
    std::wcout << L"Supported features (" << features.size() << L"):" << std::endl;
    for (const auto& feature : features) {
        std::wcout << L"  - " << feature << std::endl;
    }
}

void ExampleEnhancedRouter() {
    std::wcout << L"\n=== Enhanced Router Example ===" << std::endl;
    
    // Create enhanced router with TSReadEX integration
    tsduck_transport::EnhancedTransportStreamRouter router;
    
    // Configure TSReadEX
    tsduck_transport::TSReadEXConfig tsreadex_config;
    tsreadex_config.enabled = true;
    tsreadex_config.exclude_pids = {0x12, 0x26, 0x27};
    tsreadex_config.program_selection = -1;
    tsreadex_config.audio2_mode = 1;
    
    router.SetTSReadEXConfig(tsreadex_config);
    
    if (router.IsTSReadEXEnabled()) {
        std::wcout << L"Enhanced router ready with TSReadEX processing" << std::endl;
        
        // Get statistics
        auto stats = router.GetEnhancedBufferStats();
        std::wcout << L"Processing pipeline: " << stats.processing_pipeline << std::endl;
        std::wcout << L"TSReadEX active: " << (stats.tsreadex_active ? L"Yes" : L"No") << std::endl;
    } else {
        std::wcout << L"TSReadEX not available, using standard processing" << std::endl;
    }
}

void ExampleConfigurationPresets() {
    std::wcout << L"\n=== Configuration Presets ===" << std::endl;
    
    struct ConfigPreset {
        std::wstring name;
        std::wstring description;
        tsduck_transport::TSReadEXConfig config;
    };
    
    std::vector<ConfigPreset> presets = {
        {
            L"Stream Cleaning",
            L"Remove metadata and unnecessary streams",
            {
                .enabled = true,
                .exclude_pids = {0x12, 0x26, 0x27, 0x38, 0x39},
                .program_selection = -1
            }
        },
        {
            L"Multi-Language Audio",
            L"Ensure dual audio tracks are available",
            {
                .enabled = true,
                .program_selection = -1,
                .audio1_mode = 1,
                .audio2_mode = 3
            }
        },
        {
            L"Japanese Digital TV",
            L"ARIB caption processing with ffmpeg compatibility",
            {
                .enabled = true,
                .program_selection = -1,
                .audio1_mode = 9,
                .caption_mode = 5,
                .enable_arib_conversion = true,
                .enable_ffmpeg_bug_workaround = true,
                .enable_pts_monotonic = true
            }
        },
        {
            L"Low Latency Streaming",
            L"Optimized for live streaming with rate limiting",
            {
                .enabled = true,
                .mode = tsduck_transport::TSReadEXConfig::ProcessingMode::NONBLOCKING,
                .rate_limit_kbps = 12000,
                .timeout_seconds = 10,
                .low_latency_mode = true
            }
        }
    };
    
    for (const auto& preset : presets) {
        std::wcout << L"\nPreset: " << preset.name << std::endl;
        std::wcout << L"Description: " << preset.description << std::endl;
        std::wcout << L"Command: " << preset.config.GetCommandLine() << std::endl;
        std::wcout << L"Valid: " << (preset.config.IsValid() ? L"Yes" : L"No") << std::endl;
    }
}

int main() {
    std::wcout << L"TSReadEX Integration Examples for Tardsplaya" << std::endl;
    std::wcout << L"===========================================" << std::endl;
    
    try {
        ExampleBasicUsage();
        ExampleAdvancedConfiguration();
        ExampleEnhancedRouter();
        ExampleConfigurationPresets();
        
        std::wcout << L"\nAll examples completed successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        std::wcout << L"Error: " << std::wstring(e.what(), e.what() + strlen(e.what())) << std::endl;
        return 1;
    }
    
    return 0;
}