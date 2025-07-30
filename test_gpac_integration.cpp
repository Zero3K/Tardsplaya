#include <iostream>
#include <vector>
#include <string>

// Test program to verify GPAC integration compiles and works
extern "C" {
#include <gpac/tools.h>
#include <gpac/filters.h>
}

int main() {
    std::cout << "Testing GPAC library integration..." << std::endl;
    
    // Initialize GPAC
    GF_Err err = gf_sys_init(GF_MemTrackerNone, NULL);
    if (err != GF_OK) {
        std::cerr << "Failed to initialize GPAC: " << err << std::endl;
        return 1;
    }
    
    std::cout << "GPAC library initialized successfully!" << std::endl;
    
    // Create a filter session
    GF_FilterSession* session = gf_fs_new(0, GF_FS_SCHEDULER_LOCK_FREE, 0, NULL);
    if (!session) {
        std::cerr << "Failed to create GPAC filter session" << std::endl;
        gf_sys_close();
        return 1;
    }
    
    std::cout << "GPAC filter session created successfully!" << std::endl;
    
    // Cleanup
    gf_fs_del(session);
    gf_sys_close();
    
    std::cout << "GPAC integration test completed successfully!" << std::endl;
    return 0;
}