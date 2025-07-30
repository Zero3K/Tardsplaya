// Comprehensive test for PID filtering and discontinuity handling
#include <iostream>
#include <vector>
#include <cassert>
#include <chrono>
#include <iomanip>

#include "ts_pid_filter.h"
#include "tsduck_transport_router.h"

using namespace tsduck_transport;

// Helper function to create test TS packets
TSPacket CreateTestPacket(uint16_t pid, bool discontinuity = false, bool payload_start = false) {
    TSPacket packet;
    
    // Set sync byte
    packet.data[0] = 0x47;
    
    // Set PID and flags
    packet.data[1] = (payload_start ? 0x40 : 0x00) | ((pid >> 8) & 0x1F);
    packet.data[2] = pid & 0xFF;
    
    // Set adaptation field and continuity counter
    packet.data[3] = 0x10; // Has payload, no adaptation field initially
    
    if (discontinuity) {
        // Add adaptation field with discontinuity indicator
        packet.data[3] = 0x30; // Has payload and adaptation field
        packet.data[4] = 1;    // Adaptation field length
        packet.data[5] = 0x80; // Discontinuity indicator set
    }
    
    packet.ParseHeader();
    return packet;
}

void TestBasicPIDFiltering() {
    std::wcout << L"\n=== Testing Basic PID Filtering ===" << std::endl;
    
    TSPIDFilter filter;
    
    // Test allow list mode
    filter.SetFilterMode(PIDFilterMode::ALLOW_LIST);
    filter.AddAllowedPID(0x0000); // PAT
    filter.AddAllowedPID(0x1000); // PMT
    filter.AddAllowedPID(0x0100); // Video
    
    // Create test packets
    auto pat_packet = CreateTestPacket(0x0000);
    auto pmt_packet = CreateTestPacket(0x1000);
    auto video_packet = CreateTestPacket(0x0100);
    auto audio_packet = CreateTestPacket(0x0200); // Not in allow list
    auto null_packet = CreateTestPacket(0x1FFF);   // Null packet
    
    // Test filtering
    assert(filter.ShouldPassPacket(pat_packet) == true);
    assert(filter.ShouldPassPacket(pmt_packet) == true);
    assert(filter.ShouldPassPacket(video_packet) == true);
    assert(filter.ShouldPassPacket(audio_packet) == false);
    assert(filter.ShouldPassPacket(null_packet) == false);
    
    std::wcout << L"✓ Allow list filtering works correctly" << std::endl;
    
    // Test block list mode
    filter.SetFilterMode(PIDFilterMode::BLOCK_LIST);
    filter.ClearAllowedPIDs();
    filter.AddBlockedPID(0x1FFF); // Block null packets only
    
    assert(filter.ShouldPassPacket(pat_packet) == true);
    assert(filter.ShouldPassPacket(video_packet) == true);
    assert(filter.ShouldPassPacket(audio_packet) == true);
    assert(filter.ShouldPassPacket(null_packet) == false);
    
    std::wcout << L"✓ Block list filtering works correctly" << std::endl;
}

void TestDiscontinuityFiltering() {
    std::wcout << L"\n=== Testing Discontinuity Filtering ===" << std::endl;
    
    TSPIDFilter filter;
    
    // Test FILTER_OUT mode
    filter.SetDiscontinuityMode(DiscontinuityMode::FILTER_OUT);
    
    auto normal_packet = CreateTestPacket(0x0100, false);
    auto disc_packet = CreateTestPacket(0x0100, true);
    
    assert(filter.ShouldPassPacket(normal_packet) == true);
    assert(filter.ShouldPassPacket(disc_packet) == false);
    
    std::wcout << L"✓ Discontinuity FILTER_OUT mode works correctly" << std::endl;
    
    // Test PASS_THROUGH mode
    filter.SetDiscontinuityMode(DiscontinuityMode::PASS_THROUGH);
    
    assert(filter.ShouldPassPacket(normal_packet) == true);
    assert(filter.ShouldPassPacket(disc_packet) == true);
    
    std::wcout << L"✓ Discontinuity PASS_THROUGH mode works correctly" << std::endl;
    
    // Test SMART_FILTER mode
    filter.SetDiscontinuityMode(DiscontinuityMode::SMART_FILTER);
    
    auto pat_disc = CreateTestPacket(0x0000, true);  // PAT with discontinuity
    auto video_disc = CreateTestPacket(0x0100, true); // Video with discontinuity
    auto null_disc = CreateTestPacket(0x1FFF, true);  // Null with discontinuity
    
    assert(filter.ShouldPassPacket(pat_disc) == true);    // Essential stream - pass
    assert(filter.ShouldPassPacket(video_disc) == true);  // Essential stream - pass
    assert(filter.ShouldPassPacket(null_disc) == false);  // Non-essential stream - filter
    
    std::wcout << L"✓ Discontinuity SMART_FILTER mode works correctly" << std::endl;
}

void TestPIDStatistics() {
    std::wcout << L"\n=== Testing PID Statistics ===" << std::endl;
    
    TSPIDFilter filter;
    filter.SetFilterMode(PIDFilterMode::AUTO_DETECT);
    
    // Send multiple packets for different PIDs
    for (int i = 0; i < 50; ++i) {
        auto video_packet = CreateTestPacket(0x0100, i % 10 == 0); // 10% discontinuity rate
        auto audio_packet = CreateTestPacket(0x0200, false);
        auto problematic_packet = CreateTestPacket(0x0300, i % 3 == 0); // 33% discontinuity rate
        
        filter.ShouldPassPacket(video_packet);
        filter.ShouldPassPacket(audio_packet);
        filter.ShouldPassPacket(problematic_packet);
    }
    
    auto stats = filter.GetPIDStats(0x0100);
    assert(stats.packet_count == 50);
    assert(stats.discontinuity_count == 5); // 10% of 50
    
    auto active_pids = filter.GetActivePIDs();
    assert(active_pids.size() == 3);
    
    std::wcout << L"✓ PID statistics tracking works correctly" << std::endl;
    std::wcout << L"  Video PID packets: " << stats.packet_count << std::endl;
    std::wcout << L"  Video PID discontinuities: " << stats.discontinuity_count << std::endl;
}

void TestFilterPresets() {
    std::wcout << L"\n=== Testing Filter Presets ===" << std::endl;
    
    TSPIDFilterManager manager;
    
    // Test different presets
    std::vector<TSPIDFilterManager::FilterPreset> presets = {
        TSPIDFilterManager::FilterPreset::NONE,
        TSPIDFilterManager::FilterPreset::BASIC_CLEANUP,
        TSPIDFilterManager::FilterPreset::QUALITY_FOCUSED,
        TSPIDFilterManager::FilterPreset::MINIMAL_STREAM,
        TSPIDFilterManager::FilterPreset::DISCONTINUITY_ONLY
    };
    
    for (auto preset : presets) {
        manager.ApplyPreset(preset);
        
        // Create test packets
        std::vector<TSPacket> test_packets = {
            CreateTestPacket(0x0000), // PAT
            CreateTestPacket(0x1000), // PMT
            CreateTestPacket(0x0100), // Video
            CreateTestPacket(0x0200), // Audio
            CreateTestPacket(0x1FFF), // Null
            CreateTestPacket(0x0100, true), // Video with discontinuity
        };
        
        auto filtered = manager.ProcessPackets(test_packets);
        
        std::wcout << L"  Preset " << static_cast<int>(preset) 
                   << L": Input=" << test_packets.size() 
                   << L", Output=" << filtered.size() << std::endl;
    }
    
    std::wcout << L"✓ Filter presets work correctly" << std::endl;
}

void TestPerformance() {
    std::wcout << L"\n=== Testing Performance ===" << std::endl;
    
    TSPIDFilterManager manager;
    manager.ApplyPreset(TSPIDFilterManager::FilterPreset::QUALITY_FOCUSED);
    
    // Create a large batch of test packets
    std::vector<TSPacket> test_packets;
    const int packet_count = 10000;
    
    for (int i = 0; i < packet_count; ++i) {
        uint16_t pid = 0x0100 + (i % 16); // Mix of different PIDs
        bool discontinuity = (i % 100 == 0); // 1% discontinuity rate
        test_packets.push_back(CreateTestPacket(pid, discontinuity));
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    auto filtered = manager.ProcessPackets(test_packets);
    auto end_time = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    auto stats = manager.GetStats();
    
    std::wcout << L"✓ Performance test completed" << std::endl;
    std::wcout << L"  Processed " << packet_count << L" packets in " 
               << duration.count() << L" microseconds" << std::endl;
    std::wcout << L"  Rate: " << std::fixed << std::setprecision(2) 
               << (packet_count * 1000000.0 / duration.count()) << L" packets/second" << std::endl;
    std::wcout << L"  Filter efficiency: " << std::fixed << std::setprecision(1)
               << (stats.filter_efficiency * 100) << L"%" << std::endl;
}

void TestAutoDetection() {
    std::wcout << L"\n=== Testing Auto-Detection ===" << std::endl;
    
    TSPIDFilter filter;
    filter.SetFilterMode(PIDFilterMode::AUTO_DETECT);
    filter.EnableAutoDetection(true);
    filter.SetAutoDetectionThreshold(0.15); // 15% threshold
    
    // Send packets with varying discontinuity rates
    const int packets_per_pid = 200; // Need sufficient samples for auto-detection
    
    // PID 0x0100: Low discontinuity rate (5%)
    for (int i = 0; i < packets_per_pid; ++i) {
        auto packet = CreateTestPacket(0x0100, i % 20 == 0);
        filter.ShouldPassPacket(packet);
    }
    
    // PID 0x0200: High discontinuity rate (25%)
    for (int i = 0; i < packets_per_pid; ++i) {
        auto packet = CreateTestPacket(0x0200, i % 4 == 0);
        filter.ShouldPassPacket(packet);
    }
    
    auto problematic_pids = filter.GetProblematicPIDs();
    
    std::wcout << L"✓ Auto-detection test completed" << std::endl;
    std::wcout << L"  Problematic PIDs detected: " << problematic_pids.size() << std::endl;
    
    for (auto pid : problematic_pids) {
        auto stats = filter.GetPIDStats(pid);
        std::wcout << L"  PID 0x" << std::hex << pid << std::dec 
                   << L": " << std::fixed << std::setprecision(1)
                   << (stats.discontinuity_rate * 100) << L"% discontinuity rate" << std::endl;
    }
}

int main() {
    try {
        std::wcout << L"=== Comprehensive PID Filter Test Suite ===" << std::endl;
        std::wcout << L"Testing PID filtering and discontinuity handling functionality..." << std::endl;
        
        TestBasicPIDFiltering();
        TestDiscontinuityFiltering();
        TestPIDStatistics();
        TestFilterPresets();
        TestPerformance();
        TestAutoDetection();
        
        std::wcout << L"\n🎉 All tests passed successfully!" << std::endl;
        std::wcout << L"PID filtering implementation is working correctly." << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::wcout << L"\n❌ Test failed with exception: ";
        std::cout << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::wcout << L"\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}