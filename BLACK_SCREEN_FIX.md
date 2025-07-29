# Black Screen Fix - Media Player Command Line Arguments

## Issue Description
Some users experienced black screen with audio playing when viewing streams with certain media players (VLC, MPC-HC). This was caused by incorrect command line arguments that disabled video output or prevented proper playback initialization.

## Root Cause
1. **VLC**: The `--intf dummy` argument completely disabled the video interface
2. **MPC-HC**: Missing `/play` and `/dubdelay 0` arguments prevented proper playback initialization
3. **Hardware Acceleration**: Some systems had issues with hardware-accelerated video decoding

## Solution
Updated command line arguments for affected media players:

### VLC (Before - BROKEN)
```
vlc.exe - --intf dummy --no-one-instance
```
**Problem**: `--intf dummy` disables video output entirely

### VLC (After - FIXED)
```
vlc.exe - --intf qt --no-one-instance --video --no-video-title-show --avcodec-hw=none
```
**Changes**:
- Replaced `--intf dummy` with `--intf qt` (enables video interface)
- Added `--video` (explicitly enables video)
- Added `--no-video-title-show` (cleaner playback)
- Added `--avcodec-hw=none` (disables hardware acceleration to prevent black screen issues)

### MPC-HC (Before - INCOMPLETE)
```
mpc-hc.exe - /new /nofocus
```
**Problem**: Missing playback initialization arguments

### MPC-HC (After - FIXED)
```
mpc-hc.exe /play /dubdelay 0 - /new /nofocus
```
**Changes**:
- Added `/play` (immediate playback)
- Added `/dubdelay 0` (eliminates audio delay issues)
- Reordered parameters for optimal compatibility

## Files Modified
- `stream_pipe.cpp` - Lines 743-746 (main streaming mode)
- `tx_queue_ipc.cpp` - Lines 380-387 (TX-Queue IPC mode)

## Testing
To test the fix:
1. Try streaming with VLC - video should now display properly
2. Try streaming with MPC-HC - playback should start immediately without delays
3. If issues persist, try MPV as an alternative (unchanged, already works well)

## Fallback Options
If problems continue:
- Use MPV (recommended, most reliable)
- Check media player version compatibility
- Ensure codecs are properly installed
- Try different quality settings