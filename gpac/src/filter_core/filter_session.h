#ifndef _GF_FILTER_SESSION_H_
#define _GF_FILTER_SESSION_H_

#include <gpac/filters.h>
#include <gpac/tools.h>

// Internal filter session structure definitions
typedef struct _gf_filter_session GF_FilterSession;
typedef struct _gf_filter GF_Filter;

// Internal function declarations
GF_FilterSession *gf_fs_new_defaults(GF_FilterSessionFlags flags);
GF_Err gf_fs_stop(GF_FilterSession *session);
GF_Filter *gf_fs_load_filter(GF_FilterSession *session, const char *name, GF_Err *out_err);

#endif