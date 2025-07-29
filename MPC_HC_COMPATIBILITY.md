# MPC-HC Compatibility Guide

This document outlines the improvements made to address MPC-HC "Failed to render file" errors and provides troubleshooting steps.

## Problem Analysis

MPC-HC is particularly sensitive to:
1. **Stream format detection** - Needs proper MPEG-TS headers
2. **Data continuity** - Requires uninterrupted data flow  
3. **Buffer sizes** - Needs adequate buffering for format analysis
4. **Command line parameters** - Specific parameters for stdin streaming

## Implemented Solutions

### 1. Optimized Command Line Parameters
```cpp
// Before: "mpc-hc.exe" - /play
// After:  "mpc-hc.exe" /play /dubdelay 0 -
```
- `/dubdelay 0` - Eliminates audio delay issues
- Parameter order optimized for MPC-HC

### 2. Enhanced Buffering Strategy
- **Initial buffer**: Increased to 12 segments for MPC-HC (vs 8 for others)
- **Pipe buffer**: 4MB for MPC-HC (vs 1MB for others)  
- **Combined buffer**: Up to 3MB for MPC-HC (vs 2MB for others)
- **First-write optimization**: Send 512KB quickly for format detection

### 3. MPEG-TS Format Validation
```cpp
bool has_valid_ts_headers() const {
    // Check for TS sync bytes (0x47) at 188-byte intervals
    for (size_t i = 0; i < data.size() - 188; i += 188) {
        if (static_cast<unsigned char>(data[i]) != 0x47) {
            return false;
        }
    }
    return true;
}
```

### 4. Adaptive Timing
- **Buffering phase**: 10ms intervals for fast accumulation
- **Empty buffer**: 25ms intervals to reduce CPU usage
- **Write optimization**: Based on queue depth and buffer size

## Troubleshooting Steps

If MPC-HC still shows "Failed to render file":

### Step 1: Verify Command Line
Test different parameter combinations using `test_mpc_compatibility.bat`

### Step 2: Check Stream Format
- Ensure HLS stream contains valid MPEG-TS data
- Verify TS sync bytes (0x47) are present
- Check for proper PAT/PMT tables

### Step 3: Buffer Analysis
- Monitor initial buffer filling (should reach 1MB+ before first write)
- Check for continuous data flow (no gaps > 100ms)
- Verify combined buffer sizes (512KB-3MB range)

### Step 4: Alternative Approaches
If stdin streaming fails:
1. **Named pipes**: Use Windows named pipes instead of stdin
2. **Temporary files**: Write to temp file, open in MPC-HC
3. **HTTP server**: Serve stream via local HTTP server

## Debug Logging

Enable verbose debugging to monitor:
- Buffer sizes and timing
- TS validation results  
- MPC-HC process status
- Data transfer rates

## Known Limitations

- MPC-HC may not support all HLS stream variants
- Some TS streams may lack proper headers
- Network timing can affect buffering
- Different MPC-HC versions may behave differently

## Future Improvements

- Implement named pipe fallback for MPC-HC
- Add HTTP streaming option
- Enhance TS stream validation
- Support for different TS packet sizes (192, 204 bytes)