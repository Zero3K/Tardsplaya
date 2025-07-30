// Test compilation of GPAC embedded headers
#include "gpac/include/gpac_config.h"
#include "gpac/include/gpac/tools.h"
#include "gpac/include/gpac/filters.h"
#include "gpac/include/gpac/constants.h"

#include <iostream>

extern "C" {
    GF_Err gf_sys_init(GF_MemTrackerType mem_tracker_type, const char *profile);
    void gf_sys_close();
    GF_FilterSession* gf_fs_new(u32 nb_threads, u32 sched_type, u32 flags, const char *blacklist);
}

int main() {
    std::cout << "Testing GPAC embedded source integration..." << std::endl;
    
    // Test GPAC initialization
    GF_Err err = gf_sys_init(GF_MemTrackerNone, NULL);
    if (err == GF_OK) {
        std::cout << "GPAC initialization: SUCCESS" << std::endl;
    } else {
        std::cout << "GPAC initialization: FAILED" << std::endl;
        return 1;
    }
    
    // Test filter session creation
    GF_FilterSession* session = gf_fs_new(0, GF_FS_SCHEDULER_LOCK_FREE, 0, NULL);
    if (session) {
        std::cout << "Filter session creation: SUCCESS" << std::endl;
    } else {
        std::cout << "Filter session creation: FAILED" << std::endl;
        return 1;
    }
    
    // Cleanup
    gf_sys_close();
    std::cout << "GPAC embedded source integration test: PASSED" << std::endl;
    
    return 0;
}