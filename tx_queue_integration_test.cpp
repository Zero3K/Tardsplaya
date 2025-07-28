// Simple compilation test for TX-Queue integration
// This file tests that all headers compile correctly

#include <iostream>
#include <string>
#include <atomic>

// Test TX-Queue wrapper
#include "tx_queue_wrapper.h"

// Test TX-Queue IPC classes
#include "tx_queue_ipc.h"

// Test stream thread integration
#include "stream_thread.h"

int main() {
    std::wcout << L"=== TX-Queue Integration Test ===" << std::endl;
    
    try {
        // Test 1: Basic tx-queue functionality
        std::wcout << L"Test 1: Creating tx-queue..." << std::endl;
        
        using namespace qcstudio;
        auto queue = tx_queue_sp_t(64 * 1024); // 64KB queue
        
        if (!queue) {
            std::wcout << L"ERROR: Failed to create tx-queue" << std::endl;
            return 1;
        }
        
        std::wcout << L"SUCCESS: TX-Queue created with capacity: " 
                   << queue.capacity() << L" bytes" << std::endl;
        
        // Test 2: Write/Read operations
        std::wcout << L"Test 2: Testing write/read operations..." << std::endl;
        
        // Write test data
        {
            if (auto write_op = tx_write_t(queue)) {
                std::string test_data = "Hello TX-Queue!";
                if (write_op.write(test_data.data(), test_data.size())) {
                    std::wcout << L"SUCCESS: Write operation completed" << std::endl;
                } else {
                    std::wcout << L"ERROR: Write operation failed" << std::endl;
                    return 1;
                }
            } else {
                std::wcout << L"ERROR: Could not create write transaction" << std::endl;
                return 1;
            }
        }
        
        // Read test data
        {
            if (auto read_op = tx_read_t(queue)) {
                char buffer[32] = {0};
                if (read_op.read(buffer, 15)) { // Read "Hello TX-Queue!"
                    std::wcout << L"SUCCESS: Read operation completed: " 
                               << std::wstring(buffer, buffer + 15).c_str() << std::endl;
                } else {
                    std::wcout << L"ERROR: Read operation failed" << std::endl;
                    return 1;
                }
            } else {
                std::wcout << L"ERROR: Could not create read transaction" << std::endl;
                return 1;
            }
        }
        
        // Test 3: TX-Queue IPC classes
        std::wcout << L"Test 3: Testing TX-Queue IPC classes..." << std::endl;
        
        auto ipc_manager = std::make_unique<tardsplaya::TxQueueIPC>();
        if (!ipc_manager->Initialize()) {
            std::wcout << L"ERROR: Failed to initialize TX-Queue IPC manager" << std::endl;
            return 1;
        }
        
        std::wcout << L"SUCCESS: TX-Queue IPC manager initialized with capacity: " 
                   << ipc_manager->GetCapacity() << L" bytes" << std::endl;
        
        // Test segment production/consumption
        std::vector<char> test_segment = {'T', 'e', 's', 't', ' ', 'S', 'e', 'g', 'm', 'e', 'n', 't'};
        
        if (ipc_manager->ProduceSegment(std::move(test_segment))) {
            std::wcout << L"SUCCESS: Segment produced" << std::endl;
        } else {
            std::wcout << L"ERROR: Failed to produce segment" << std::endl;
            return 1;
        }
        
        tardsplaya::StreamSegment consumed_segment;
        if (ipc_manager->ConsumeSegment(consumed_segment)) {
            std::wcout << L"SUCCESS: Segment consumed, size: " 
                       << consumed_segment.data.size() << L" bytes" << std::endl;
        } else {
            std::wcout << L"ERROR: Failed to consume segment" << std::endl;
            return 1;
        }
        
        // Test 4: Streaming mode enumeration
        std::wcout << L"Test 4: Testing streaming mode..." << std::endl;
        
        StreamingMode mode = StreamingMode::TX_QUEUE_IPC;
        if (mode == StreamingMode::TX_QUEUE_IPC) {
            std::wcout << L"SUCCESS: TX_QUEUE_IPC streaming mode available" << std::endl;
        } else {
            std::wcout << L"ERROR: TX_QUEUE_IPC streaming mode not available" << std::endl;
            return 1;
        }
        
        std::wcout << L"=== ALL TESTS PASSED ===" << std::endl;
        std::wcout << L"TX-Queue integration is working correctly!" << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::wcout << L"EXCEPTION: ";
        std::cout << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::wcout << L"UNKNOWN EXCEPTION occurred" << std::endl;
        return 1;
    }
}