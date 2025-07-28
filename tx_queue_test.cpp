// Simple test for TX-Queue integration
#include <windows.h>
#include <iostream>
#include "tx-queue/tx-queue.h"

int main() {
    try {
        using namespace qcstudio;
        
        // Create a simple tx-queue
        auto queue = tx_queue_sp_t(8 * 1024); // 8KB queue
        
        if (!queue) {
            std::wcout << L"Failed to create tx-queue" << std::endl;
            return 1;
        }
        
        std::wcout << L"TX-Queue created successfully with capacity: " 
                   << queue.capacity() << L" bytes" << std::endl;
        
        // Test write operation
        {
            if (auto write_op = tx_write_t(queue)) {
                int test_value = 42;
                if (write_op.write(test_value)) {
                    std::wcout << L"Write operation successful" << std::endl;
                } else {
                    std::wcout << L"Write operation failed" << std::endl;
                }
            }
        }
        
        // Test read operation
        {
            if (auto read_op = tx_read_t(queue)) {
                int read_value = 0;
                if (read_op.read(read_value)) {
                    std::wcout << L"Read operation successful, value: " << read_value << std::endl;
                } else {
                    std::wcout << L"Read operation failed" << std::endl;
                }
            }
        }
        
        std::wcout << L"TX-Queue test completed successfully" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::wcout << L"Exception: ";
        std::cout << e.what() << std::endl;
        return 1;
    }
}