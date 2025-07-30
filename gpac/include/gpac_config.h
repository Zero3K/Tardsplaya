#ifndef _GPAC_CONFIG_H_
#define _GPAC_CONFIG_H_

// Minimal GPAC configuration for Tardsplaya integration
// This provides the essential configuration defines for GPAC compilation

// Platform detection
#ifdef _WIN32
#define WIN32 1
#define GPAC_CONFIG_WIN32 1
#define GPAC_DISABLE_IPV6 1
#else
#define UNIX 1
#define GPAC_CONFIG_LINUX 1
// Define platform for configuration header
#ifndef GPAC_CONFIG_STATIC
#define GPAC_CONFIG_STATIC 1
#endif
#ifndef GPAC_STATIC_MODULES
#define GPAC_STATIC_MODULES 1
#endif
#endif

// Essential features for HLS processing
#define GPAC_HAS_SSL 1
#define GPAC_HAS_JPEG 1
#define GPAC_HAS_PNG 1

// Disable optional features to minimize dependencies
#define GPAC_DISABLE_TTML 1
#define GPAC_DISABLE_SVG 1
#define GPAC_DISABLE_VRML 1
#define GPAC_DISABLE_X3D 1
#define GPAC_DISABLE_PLAYER 1
#define GPAC_DISABLE_COMPOSITOR 1

// Enable required modules for MP4/HLS support
#define GPAC_HAS_MPD 1
#define GPAC_MINIMAL_ODF 1

// Memory management
#define GPAC_MEMORY_TRACKING 0

// Version information
#define GPAC_VERSION "2.5.0-dev"
#define GPAC_VERSION_MAJOR 2
#define GPAC_VERSION_MINOR 5
#define GPAC_VERSION_MICRO 0

#endif /* _GPAC_CONFIG_H_ */