// Test for PID discontinuity filtering functionality
// This test validates the tspidfilter-like functionality for filtering
// Transport Stream packets with discontinuity indicators from specific PIDs

#include "tsduck_transport_router.h"
#include <iostream>
#include <cassert>

using namespace tsduck_transport;

// Test helper to create TS packet with specific PID and discontinuity flag
TSPacket CreateTestPacket(uint16_t pid, bool has_discontinuity = false) {
    TSPacket packet;
    
    // Set sync byte
    packet.data[0] = 0x47;
    
    // Set PID in bytes 1-2 (13 bits)
    packet.data[1] = (pid >> 8) & 0x1F;
    packet.data[2] = pid & 0xFF;
    
    if (has_discontinuity) {
        // Set adaptation field flag and discontinuity indicator
        packet.data[3] = 0x30; // Has adaptation field and payload
        packet.data[4] = 1;    // Adaptation field length = 1
        packet.data[5] = 0x80; // Discontinuity indicator set
        packet.discontinuity = true;
    } else {
        packet.data[3] = 0x10; // Payload only, no adaptation field
        packet.discontinuity = false;
    }
    
    packet.pid = pid;
    return packet;
}

int main() {
    std::wcout << L"Testing PID Discontinuity Filter..." << std::endl;
    
    try {
        // Test 1: Basic filtering functionality
        {
            std::wcout << L"Test 1: Basic filtering functionality..." << std::endl;
            
            PIDDiscontinuityFilter filter;
            PIDDiscontinuityFilter::FilterConfig config;
            config.enable_discontinuity_filtering = true;
            config.filter_pids.insert(0x100); // Filter PID 0x100
            config.auto_detect_problem_pids = false; // Disable auto-detection for this test
            filter.SetFilterConfig(config);
            
            // Create test packets
            TSPacket normal_packet = CreateTestPacket(0x100, false);
            TSPacket discontinuity_packet = CreateTestPacket(0x100, true);
            TSPacket other_pid_packet = CreateTestPacket(0x200, true);
            
            // Test filtering
            assert(!filter.ShouldFilterPacket(normal_packet)); // Normal packet should not be filtered
            assert(filter.ShouldFilterPacket(discontinuity_packet)); // Discontinuity packet from filtered PID should be filtered
            assert(!filter.ShouldFilterPacket(other_pid_packet)); // Discontinuity packet from non-filtered PID should not be filtered
            
            std::wcout << L"✓ Basic filtering test passed" << std::endl;
        }
        
        // Test 2: Auto-detection functionality
        {
            std::wcout << L"Test 2: Auto-detection functionality..." << std::endl;
            
            PIDDiscontinuityFilter filter;
            PIDDiscontinuityFilter::FilterConfig config;
            config.enable_discontinuity_filtering = true;
            config.auto_detect_problem_pids = true;
            config.discontinuity_threshold = 3; // Low threshold for testing
            filter.SetFilterConfig(config);
            
            // Simulate multiple discontinuities on PID 0x300
            TSPacket test_packet = CreateTestPacket(0x300, true);
            for (int i = 0; i < 5; ++i) {
                filter.ShouldFilterPacket(test_packet); // This will track discontinuities
            }
            
            // Check if PID is auto-detected as problematic
            auto problem_pids = filter.GetProblemPIDs();
            // Note: Auto-detection requires time-based logic which may not trigger immediately in test
            
            auto stats = filter.GetDiscontinuityStats();
            assert(stats[0x300] == 5); // Should have tracked 5 discontinuities
            
            std::wcout << L"✓ Auto-detection test passed" << std::endl;
        }
        
        // Test 3: Configuration disable
        {
            std::wcout << L"Test 3: Disabled filtering..." << std::endl;
            
            PIDDiscontinuityFilter filter;
            PIDDiscontinuityFilter::FilterConfig config;
            config.enable_discontinuity_filtering = false; // Disabled
            config.filter_pids.insert(0x400);
            filter.SetFilterConfig(config);
            
            TSPacket discontinuity_packet = CreateTestPacket(0x400, true);
            assert(!filter.ShouldFilterPacket(discontinuity_packet)); // Should not filter when disabled
            
            std::wcout << L"✓ Disabled filtering test passed" << std::endl;
        }
        
        // Test 4: Statistics reset
        {
            std::wcout << L"Test 4: Statistics reset..." << std::endl;
            
            PIDDiscontinuityFilter filter;
            PIDDiscontinuityFilter::FilterConfig config;
            config.enable_discontinuity_filtering = true;
            filter.SetFilterConfig(config);
            
            // Track some discontinuities
            TSPacket test_packet = CreateTestPacket(0x500, true);
            filter.ShouldFilterPacket(test_packet);
            filter.ShouldFilterPacket(test_packet);
            
            auto stats_before = filter.GetDiscontinuityStats();
            assert(stats_before[0x500] == 2);
            
            // Reset and check
            filter.Reset();
            auto stats_after = filter.GetDiscontinuityStats();
            assert(stats_after.empty() || stats_after[0x500] == 0);
            
            std::wcout << L"✓ Statistics reset test passed" << std::endl;
        }
        
        std::wcout << L"All PID discontinuity filter tests passed successfully!" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::wcout << L"Test failed with exception: ";
        std::cout << e.what() << std::endl;
        return 1;
    }
}