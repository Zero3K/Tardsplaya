// Example usage of PID-based discontinuity filtering in Tardsplaya
// This demonstrates how to configure and use the tspidfilter-like functionality

#include "tsduck_transport_router.h"
#include <iostream>

using namespace tsduck_transport;

void ExamplePIDFilterConfiguration() {
    std::wcout << L"Configuring PID-based discontinuity filtering..." << std::endl;
    
    // Create transport stream router
    TransportStreamRouter router;
    
    // Configure the router with PID filtering enabled
    TransportStreamRouter::RouterConfig config;
    
    // Basic streaming configuration
    config.player_path = L"mpv.exe";
    config.player_args = L"-";
    config.buffer_size_packets = 15000;
    config.low_latency_mode = true;
    
    // Configure PID-based discontinuity filtering (tspidfilter-like functionality)
    config.pid_filter_config.enable_discontinuity_filtering = true;
    
    // Option 1: Manually specify PIDs to filter discontinuity packets from
    // These are commonly auxiliary data streams that can cause playback issues
    config.pid_filter_config.filter_pids.insert(0x1FFE); // Null packets
    config.pid_filter_config.filter_pids.insert(0x1FFF); // Stuffing packets
    
    // Option 2: Enable automatic detection of problematic PIDs
    config.pid_filter_config.auto_detect_problem_pids = true;
    config.pid_filter_config.discontinuity_threshold = 5; // Filter PIDs with >5 discontinuities per minute
    
    // Enable logging to see filtering activity
    config.pid_filter_config.log_discontinuity_stats = true;
    
    std::wcout << L"PID filter configuration:" << std::endl;
    std::wcout << L"- Filtering enabled: " << (config.pid_filter_config.enable_discontinuity_filtering ? L"Yes" : L"No") << std::endl;
    std::wcout << L"- Manual filter PIDs: " << config.pid_filter_config.filter_pids.size() << L" PIDs" << std::endl;
    std::wcout << L"- Auto-detection: " << (config.pid_filter_config.auto_detect_problem_pids ? L"Yes" : L"No") << std::endl;
    std::wcout << L"- Threshold: " << config.pid_filter_config.discontinuity_threshold << L" discontinuities/min" << std::endl;
    
    // The router will now filter discontinuity packets from the specified PIDs
    // This helps resolve playback issues caused by discontinuities in auxiliary data streams
    
    std::wcout << L"Configuration complete. Use router.StartRouting() to begin streaming with PID filtering." << std::endl;
}

void ExampleMonitoringDiscontinuities() {
    std::wcout << L"Example: Monitoring discontinuity statistics..." << std::endl;
    
    TransportStreamRouter router;
    
    // During streaming, you can monitor discontinuity statistics:
    auto stats = router.GetBufferStats();
    
    std::wcout << L"Discontinuity statistics by PID:" << std::endl;
    for (const auto& [pid, count] : stats.discontinuity_count_by_pid) {
        std::wcout << L"  PID 0x" << std::hex << pid << L": " << std::dec << count << L" discontinuities" << std::endl;
    }
    
    std::wcout << L"Auto-detected problem PIDs:" << std::endl;
    for (uint16_t pid : stats.problem_pids) {
        std::wcout << L"  PID 0x" << std::hex << pid << std::dec << L" (auto-filtered)" << std::endl;
    }
    
    std::wcout << L"Total filtered packets: " << stats.total_filtered_packets << std::endl;
}

int main() {
    std::wcout << L"=== Tardsplaya PID Discontinuity Filter Examples ===" << std::endl;
    std::wcout << std::endl;
    
    ExamplePIDFilterConfiguration();
    std::wcout << std::endl;
    
    ExampleMonitoringDiscontinuities();
    std::wcout << std::endl;
    
    std::wcout << L"These examples show how to use the tspidfilter-like functionality" << std::endl;
    std::wcout << L"to improve stream quality by filtering problematic discontinuity packets." << std::endl;
    
    return 0;
}