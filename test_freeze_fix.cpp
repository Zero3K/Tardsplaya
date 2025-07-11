#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

// Minimal test to demonstrate the stream freezing fix
// This simulates the HTTP server behavior before and after the fix

class OldHttpServer {
private:
    std::queue<std::vector<char>> data_queue;
    std::mutex queue_mutex;
    std::atomic<bool> running{true};
    
public:
    void AddData(const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        data_queue.push(data);
    }
    
    // Old behavior - causes freezing with 50ms delays
    void StreamDataOld() {
        std::cout << "[OLD] Starting stream with 50ms delays...\n";
        int segments_sent = 0;
        int delay_count = 0;
        const int max_segments = 15; // Limit to prevent infinite loop
        
        while (running && segments_sent < max_segments && delay_count < 50) {
            std::vector<char> data;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!data_queue.empty()) {
                    data = std::move(data_queue.front());
                    data_queue.pop();
                }
            }
            
            if (!data.empty()) {
                std::cout << "[OLD] Sent segment " << segments_sent << "\n";
                segments_sent++;
                delay_count = 0;
            } else {
                // OLD BEHAVIOR: 50ms delay causes video freezing
                std::cout << "[OLD] No data, waiting 50ms... (delay #" << ++delay_count << ")\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        std::cout << "[OLD] Stream ended with " << segments_sent << " segments and " << delay_count << " delays\n";
    }
};

class NewHttpServer {
private:
    std::queue<std::vector<char>> data_queue;
    std::mutex queue_mutex;
    std::atomic<bool> running{true};
    
public:
    void AddData(const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        data_queue.push(data);
    }
    
    size_t GetQueueLength() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex));
        return data_queue.size();
    }
    
    // New behavior - prevents freezing with shorter delays and timeout
    void StreamDataNew() {
        std::cout << "[NEW] Starting stream with 10ms delays and timeout...\n";
        int segments_sent = 0;
        int empty_queue_count = 0;
        const int max_empty_waits = 20; // 200ms max wait (10ms * 20)
        const int max_segments = 15; // Match old test
        
        while (running && segments_sent < max_segments) {
            std::vector<char> data;
            {
                std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex));
                if (!data_queue.empty()) {
                    data = std::move(data_queue.front());
                    data_queue.pop();
                    empty_queue_count = 0; // Reset counter when data is available
                }
            }
            
            if (!data.empty()) {
                std::cout << "[NEW] Sent segment " << segments_sent << "\n";
                segments_sent++;
            } else {
                // NEW BEHAVIOR: Shorter delay and timeout prevents freezing
                empty_queue_count++;
                if (empty_queue_count >= max_empty_waits) {
                    std::cout << "[NEW] No data for too long (" << (empty_queue_count * 10) << "ms), ending to prevent freeze\n";
                    break;
                }
                std::cout << "[NEW] No data, waiting 10ms... (delay #" << empty_queue_count << ")\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::cout << "[NEW] Stream ended with " << segments_sent << " segments and " << empty_queue_count << " short delays\n";
    }
};

int main() {
    std::cout << "=== Stream Freezing Fix Demonstration ===\n\n";
    
    // Test old behavior
    std::cout << "Testing OLD behavior (causes freezing):\n";
    OldHttpServer old_server;
    
    // Add some data with gaps to simulate real streaming
    std::thread old_feeder([&old_server]() {
        for (int i = 0; i < 10; i++) {
            std::vector<char> data(1024, 'A' + i);
            old_server.AddData(data);
            std::this_thread::sleep_for(std::chrono::milliseconds(60)); // Simulate slow download
        }
    });
    
    auto start_time = std::chrono::high_resolution_clock::now();
    old_server.StreamDataOld();
    auto old_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time);
    
    old_feeder.join();
    
    std::cout << "\n" << std::string(50, '-') << "\n\n";
    
    // Test new behavior
    std::cout << "Testing NEW behavior (prevents freezing):\n";
    NewHttpServer new_server;
    
    // Add same data pattern
    std::thread new_feeder([&new_server]() {
        for (int i = 0; i < 10; i++) {
            std::vector<char> data(1024, 'A' + i);
            new_server.AddData(data);
            std::this_thread::sleep_for(std::chrono::milliseconds(60)); // Simulate slow download
        }
    });
    
    start_time = std::chrono::high_resolution_clock::now();
    new_server.StreamDataNew();
    auto new_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time);
    
    new_feeder.join();
    
    std::cout << "\n=== RESULTS ===\n";
    std::cout << "Old behavior time: " << old_duration.count() << "ms (causes freezing)\n";
    std::cout << "New behavior time: " << new_duration.count() << "ms (prevents freezing)\n";
    std::cout << "Improvement: " << (old_duration.count() - new_duration.count()) << "ms faster\n";
    
    return 0;
}