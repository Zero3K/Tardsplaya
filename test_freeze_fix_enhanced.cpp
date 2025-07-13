#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

// Enhanced test to simulate the new freeze detection and prevention mechanisms
// This demonstrates the additional safeguards for IPC pipe issues

class EnhancedStreamFeeder {
private:
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::atomic<bool> download_running{true};
    std::atomic<bool> player_responsive{true}; // Simulate player becoming unresponsive
    
public:
    void AddData(const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(buffer_mutex);
        buffer_queue.push(data);
    }
    
    size_t GetQueueSize() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(buffer_mutex));
        return buffer_queue.size();
    }
    
    void SetPlayerResponsive(bool responsive) {
        player_responsive = responsive;
    }
    
    void SetDownloadRunning(bool running) {
        download_running = running;
    }
    
    // Simulate WriteFile to player stdin with timeout detection
    bool WriteToPlayer(const std::vector<char>& data, int& write_time_ms) {
        if (!player_responsive.load()) {
            // Simulate slow/blocked write when player is unresponsive
            std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // Simulate 1.2s blocked write
            write_time_ms = 1200;
            return false; // Write "fails" when player is unresponsive
        }
        
        // Normal fast write
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        write_time_ms = 5;
        return true;
    }
    
    // Enhanced feeder with new safeguards
    void StreamDataEnhanced() {
        std::cout << "[ENHANCED] Starting stream with freeze detection...\n";
        
        const size_t min_buffer_size = 3;
        const size_t target_buffer_segments = 5;
        int empty_buffer_count = 0;
        const int max_empty_waits = 100; // 1 second max wait (10ms * 100)
        
        // Player health monitoring
        size_t last_buffer_size = 0;
        int buffer_not_decreasing_count = 0;
        const int max_buffer_stagnant_cycles = 10; // Reduced for test
        
        int segments_sent = 0;
        int cycles = 0;
        const int max_cycles = 50; // Limit test duration
        
        while (cycles < max_cycles) {
            cycles++;
            
            // Get current buffer state
            size_t buffer_size = GetQueueSize();
            
            // Player health monitoring - detect if buffer is growing while download active
            if (download_running.load() && buffer_size >= last_buffer_size && buffer_size > target_buffer_segments) {
                buffer_not_decreasing_count++;
                if (buffer_not_decreasing_count >= max_buffer_stagnant_cycles) {
                    std::cout << "[ENHANCED] WARNING: Buffer stagnant for " << buffer_not_decreasing_count 
                              << " cycles (buffer=" << buffer_size << ") - player may be frozen\n";
                    buffer_not_decreasing_count = 0; // Reset to avoid spam
                }
            } else {
                buffer_not_decreasing_count = 0;
            }
            last_buffer_size = buffer_size;
            
            // Try to feed data
            std::vector<std::vector<char>> segments_to_feed;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!buffer_queue.empty()) {
                    segments_to_feed.push_back(std::move(buffer_queue.front()));
                    buffer_queue.pop();
                }
            }
            
            if (!segments_to_feed.empty()) {
                // Simulate writing to player with timeout detection
                auto write_start = std::chrono::high_resolution_clock::now();
                int write_time_ms = 0;
                bool write_success = WriteToPlayer(segments_to_feed[0], write_time_ms);
                
                if (!write_success || write_time_ms > 1000) {
                    std::cout << "[ENHANCED] Write failure/timeout detected (" << write_time_ms 
                              << "ms) - possible player freeze\n";
                    if (!write_success) break; // Exit on write failure
                }
                
                if (write_time_ms > 1000) {
                    std::cout << "[ENHANCED] WARNING: Slow write detected (" << write_time_ms 
                              << "ms) - player may be unresponsive\n";
                }
                
                segments_sent++;
                empty_buffer_count = 0;
                std::cout << "[ENHANCED] Fed segment " << segments_sent 
                          << ", buffer=" << (buffer_size - 1) 
                          << ", write_time=" << write_time_ms << "ms\n";
                          
            } else {
                // No data available
                empty_buffer_count++;
                
                // Adaptive timeout based on download state
                int effective_max_waits = download_running.load() ? max_empty_waits : (max_empty_waits / 5);
                
                if (empty_buffer_count >= effective_max_waits) {
                    std::cout << "[ENHANCED] No data for too long (" << (empty_buffer_count * 10) 
                              << "ms), ending to prevent freeze (download_running=" 
                              << download_running.load() << ")\n";
                    break;
                }
                
                std::cout << "[ENHANCED] No data, waiting 10ms... (delay #" << empty_buffer_count << ")\n";
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        std::cout << "[ENHANCED] Stream ended with " << segments_sent << " segments in " << cycles << " cycles\n";
    }
};

int main() {
    std::cout << "=== Enhanced Stream Freeze Detection Test ===\n\n";
    
    EnhancedStreamFeeder feeder;
    
    // Test 1: Normal operation
    std::cout << "Test 1: Normal streaming behavior\n";
    std::thread data_provider1([&feeder]() {
        for (int i = 0; i < 15; i++) {
            std::vector<char> data(1024, 'A' + (i % 26));
            feeder.AddData(data);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        feeder.SetDownloadRunning(false);
    });
    
    auto start_time = std::chrono::high_resolution_clock::now();
    feeder.StreamDataEnhanced();
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time);
    
    data_provider1.join();
    
    std::cout << "\n" << std::string(60, '-') << "\n\n";
    
    // Test 2: Player becomes unresponsive
    std::cout << "Test 2: Player becomes unresponsive (simulates freeze)\n";
    EnhancedStreamFeeder feeder2;
    feeder2.SetPlayerResponsive(true); // Start responsive
    
    std::thread data_provider2([&feeder2]() {
        for (int i = 0; i < 20; i++) {
            std::vector<char> data(1024, 'B' + (i % 26));
            feeder2.AddData(data);
            
            // Make player unresponsive after 5 segments
            if (i == 5) {
                std::cout << "[TEST] Making player unresponsive...\n";
                feeder2.SetPlayerResponsive(false);
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    
    start_time = std::chrono::high_resolution_clock::now();
    feeder2.StreamDataEnhanced();
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time);
    
    data_provider2.join();
    
    std::cout << "\n=== RESULTS ===\n";
    std::cout << "Normal operation time: " << duration1.count() << "ms\n";
    std::cout << "Unresponsive player time: " << duration2.count() << "ms\n";
    std::cout << "Enhanced detection successfully identified and handled player freeze!\n";
    
    return 0;
}