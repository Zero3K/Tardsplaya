#ifndef _GF_FILTER_DEV_H_
#define _GF_FILTER_DEV_H_

#include <gpac/filters.h>

// Internal filter development structures
// This provides the internal structures needed by filter implementations

typedef struct _gf_filter_register {
    const char *name;
    u32 flags;
    GF_FilterCapability *caps;
    u32 nb_caps;
} GF_FilterRegister;

// Internal filter structure definitions
struct _gf_filter {
    const GF_FilterRegister *freg;
    GF_FilterSession *session;
    char *name;
    u32 id;
};

struct _gf_filter_session {
    GF_List *filters;
    u32 nb_threads;
    Bool run_status;
};

#endif