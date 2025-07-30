// Minimal GPAC initialization stubs for Tardsplaya integration
// This provides basic functionality for the GPAC API calls used in gpac_decoder.cpp

#include <gpac/setup.h>
#include <gpac/tools.h>
#include <gpac/filters.h>
#include <gpac/isomedia.h>
#include <gpac/constants.h>

// Global initialization flag
static int gpac_initialized = 0;

// Initialize GPAC library
GF_Err gf_sys_init(GF_MemTrackerType mem_tracker_type, const char *profile) {
    if (gpac_initialized) {
        return GF_OK;
    }
    
    // Minimal initialization
    gpac_initialized = 1;
    return GF_OK;
}

// Close GPAC library
void gf_sys_close() {
    gpac_initialized = 0;
}

// Create filter session
GF_FilterSession* gf_fs_new(u32 nb_threads, u32 sched_type, u32 flags, const char *blacklist) {
    // Return a dummy filter session pointer
    // In a real implementation, this would allocate and initialize a filter session
    return (GF_FilterSession*)0x12345678; // Dummy pointer
}

// Delete filter session
void gf_fs_del(GF_FilterSession *session) {
    // In a real implementation, this would cleanup the filter session
    // For now, just return
}

// Load source filter
GF_Filter* gf_fs_load_source(GF_FilterSession *session, const char *url, const char *parent_url, const char *opts, GF_Err *out_err) {
    if (out_err) *out_err = GF_OK;
    
    // Return a dummy filter pointer
    // In a real implementation, this would create and configure a source filter for the URL
    return (GF_Filter*)0x87654321; // Dummy pointer
}

// Load destination filter
GF_Filter* gf_fs_load_destination(GF_FilterSession *session, const char *url, const char *opts, const char *parent_url, GF_Err *out_err) {
    if (out_err) *out_err = GF_OK;
    
    // Return a dummy filter pointer
    // In a real implementation, this would create and configure a destination filter
    return (GF_Filter*)0x13579246; // Dummy pointer
}

// Run filter session
GF_Err gf_fs_run(GF_FilterSession *session) {
    // In a real implementation, this would run the filter graph
    // For now, simulate successful completion
    return GF_EOS; // End of stream (successful completion)
}