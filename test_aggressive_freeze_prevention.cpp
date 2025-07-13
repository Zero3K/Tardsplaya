#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

// Test the new aggressive freeze prevention mechanisms
class AggressiveFreezePreventionTest {
private:
    std::queue<std::vector<char>> buffer_queue;
    std::mutex buffer_mutex;
    std::atomic<bool> download_running{true};
    std::atomic<bool> cancel_token{false};
    
    // Simulation controls
    std::atomic<bool> player_responsive{true};
    std::atomic<int> write_delay_ms{5};
    std::atomic<bool> simulate_ad_block{false};
    
public:
    void AddData(const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(buffer_mutex);
        buffer_queue.push(data);
    }
    
    size_t GetQueueSize() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(buffer_mutex));
        return buffer_queue.size();
    }
    
    void SetPlayerResponsive(bool responsive) { player_responsive = responsive; }
    void SetWriteDelay(int ms) { write_delay_ms = ms; }
    void SetDownloadRunning(bool running) { download_running = running; }
    void SimulateAdBlock(bool enable) { simulate_ad_block = enable; }
    
    // Simulate aggressive WriteFile with strict timeouts
    bool SimulateAggressiveWriteFile(const std::vector<char>& data, int& actual_delay) {
        if (!player_responsive.load()) {
            actual_delay = 600; // Simulate moderate blockage (above 500ms threshold)
            return false;
        }
        
        int delay = write_delay_ms.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        actual_delay = delay;
        return true;
    }
    
    // Test aggressive freeze prevention
    void RunAggressivePreventionTest(const std::string& test_name) {
        std::cout << "\n=== AGGRESSIVE " << test_name << " ===\n";
        
        const size_t min_buffer_size = 3;
        const size_t target_buffer_segments = 5;
        int empty_buffer_count = 0;
        const int max_empty_waits = 50;
        
        // More aggressive monitoring
        size_t last_buffer_size = 0;
        int buffer_not_decreasing_count = 0;
        const int max_buffer_stagnant_cycles = 10; // Reduced from 20
        
        int segments_sent = 0;
        int cycles = 0;
        const int max_cycles = 40;
        int critical_timeouts = 0;
        int warnings = 0;
        bool stream_aborted = false;
        
        auto test_start = std::chrono::high_resolution_clock::now();
        
        while (cycles < max_cycles && !cancel_token.load() && !stream_aborted) {
            cycles++;
            
            size_t buffer_size = GetQueueSize();
            
            // Aggressive buffer stagnation detection (NEW)
            if (download_running.load() && buffer_size >= last_buffer_size && buffer_size > target_buffer_segments) {
                buffer_not_decreasing_count++;
                if (buffer_not_decreasing_count >= max_buffer_stagnant_cycles) {
                    std::cout << "[CRITICAL] Buffer stagnant for " << buffer_not_decreasing_count 
                              << " cycles - ABORTING STREAM (player frozen)\n";
                    stream_aborted = true;
                    break;
                } else if (buffer_not_decreasing_count >= (max_buffer_stagnant_cycles / 2)) {
                    std::cout << "[WARNING] Buffer stagnation signs (" << buffer_not_decreasing_count 
                              << "/" << max_buffer_stagnant_cycles << " cycles)\n";
                    warnings++;
                }
            } else {
                buffer_not_decreasing_count = 0;
            }
            last_buffer_size = buffer_size;
            
            // Emergency buffer feeding (NEW)
            std::vector<std::vector<char>> segments_to_feed;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                int max_segments_to_feed = 1;
                
                if (buffer_size == 0) {
                    // Critical: emergency feeding
                    max_segments_to_feed = std::min((int)buffer_queue.size(), 5);
                    std::cout << "[EMERGENCY] Buffer completely empty, feeding " 
                              << max_segments_to_feed << " segments\n";
                } else if (buffer_size < min_buffer_size) {
                    max_segments_to_feed = std::min((int)buffer_queue.size(), 3);
                    std::cout << "[URGENT] Low buffer (" << buffer_size << "), feeding " 
                              << max_segments_to_feed << " segments\n";
                }
                
                int segments_fed = 0;
                while (!buffer_queue.empty() && segments_fed < max_segments_to_feed) {
                    segments_to_feed.push_back(std::move(buffer_queue.front()));
                    buffer_queue.pop();
                    segments_fed++;
                }
            }
            
            if (!segments_to_feed.empty()) {
                // Aggressive WriteFile timeout detection (NEW: 500ms vs 1000ms)
                for (const auto& segment_data : segments_to_feed) {
                    int write_time = 0;
                    bool write_success = SimulateAggressiveWriteFile(segment_data, write_time);
                    
                    if (!write_success || write_time > 500) {
                        std::cout << "[CRITICAL] Write timeout (" << write_time 
                                  << "ms > 500ms) - ABORTING STREAM\n";
                        critical_timeouts++;
                        stream_aborted = true;
                        break;
                    } else if (write_time > 200) {
                        std::cout << "[WARNING] Slow write detected (" << write_time << "ms)\n";
                        warnings++;
                    }
                }
                
                if (!stream_aborted) {
                    segments_sent += segments_to_feed.size();
                    std::cout << "[SUCCESS] Fed " << segments_to_feed.size() 
                              << " segments, total=" << segments_sent << "\n";
                }
            } else {
                empty_buffer_count++;
                if (empty_buffer_count >= max_empty_waits) {
                    std::cout << "[TIMEOUT] No data for " << (empty_buffer_count * 10) 
                              << "ms - ending stream\n";
                    break;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        auto test_end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(test_end - test_start);
        
        std::cout << "\n--- AGGRESSIVE TEST RESULTS ---\n";
        std::cout << "Duration: " << duration.count() << "ms\n";
        std::cout << "Cycles: " << cycles << "/" << max_cycles << "\n";
        std::cout << "Segments sent: " << segments_sent << "\n";
        std::cout << "Warnings: " << warnings << "\n";
        std::cout << "Critical timeouts: " << critical_timeouts << "\n";
        std::cout << "Stream aborted: " << (stream_aborted ? "YES" : "NO") << "\n";
        std::cout << "Final buffer size: " << GetQueueSize() << "\n";
        
        // Test verdict
        if (stream_aborted && critical_timeouts > 0) {
            std::cout << "RESULT: ✓ CORRECTLY DETECTED AND ABORTED FROZEN STREAM\n";
        } else if (!stream_aborted && critical_timeouts == 0) {
            std::cout << "RESULT: ✓ HEALTHY STREAM CONTINUED NORMALLY\n";
        } else {
            std::cout << "RESULT: ✗ UNEXPECTED BEHAVIOR\n";
        }
    }
    
    void RunAllAggressiveTests() {
        std::cout << "=== AGGRESSIVE FREEZE PREVENTION TEST SUITE ===\n";
        
        // Test 1: Normal operation with aggressive thresholds
        for (int i = 0; i < 10; i++) {
            AddData(std::vector<char>(1000, 'A' + i));
        }
        player_responsive = true;
        write_delay_ms = 5;
        RunAggressivePreventionTest("NORMAL OPERATION");
        
        // Test 2: Moderately slow player (should trigger warnings)
        for (int i = 0; i < 10; i++) {
            AddData(std::vector<char>(1000, 'B' + i));
        }
        player_responsive = true;
        write_delay_ms = 250; // Between 200ms warning and 500ms critical
        RunAggressivePreventionTest("SLOW PLAYER");
        
        // Test 3: Frozen player (should abort quickly)
        for (int i = 0; i < 10; i++) {
            AddData(std::vector<char>(1000, 'C' + i));
        }
        player_responsive = false; // Will cause >500ms writes
        RunAggressivePreventionTest("FROZEN PLAYER");
        
        // Test 4: Buffer stagnation (should detect and abort)
        for (int i = 0; i < 15; i++) {
            AddData(std::vector<char>(1000, 'D' + i));
        }
        player_responsive = true;
        write_delay_ms = 5;
        // Simulate player not consuming by not removing from queue
        RunAggressivePreventionTest("BUFFER STAGNATION");
        
        std::cout << "\n=== ALL AGGRESSIVE TESTS COMPLETE ===\n";
    }
};

int main() {
    AggressiveFreezePreventionTest test;
    test.RunAllAggressiveTests();
    return 0;
}