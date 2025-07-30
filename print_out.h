#ifndef __PRINT_OUT_H__
#define __PRINT_OUT_H__

#include <stdio.h>

#define DO_NOTHING do {} while(0)

// MSVC-compatible variadic macros
#ifdef DEBUG
    #define ERR(...) printf("Error: " __VA_ARGS__)
    #define OUT(...) printf(__VA_ARGS__)
    #define DBG(...) printf(__VA_ARGS__)
#else
    #define ERR(...) printf("Error: " __VA_ARGS__)
    #define OUT(...) printf(__VA_ARGS__)
    #define DBG(...) DO_NOTHING
#endif

#endif // __PRINT_OUT_H__
