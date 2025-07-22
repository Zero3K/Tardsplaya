# Testing PTS Discontinuity Fix

## Quick Test

To verify the PTS discontinuity correction logic is working:

```bash
# Compile and run the standalone test
g++ -std=c++17 -o standalone_pts_test standalone_pts_test.cpp
./standalone_pts_test
```

Expected output:
```
PTS Discontinuity Correction Test Suite
=======================================

=== Testing PTS Encoding/Decoding ===
Original PTS: 0 -> Encoded -> Extracted: 0 [PASS]
Original PTS: 90000 -> Encoded -> Extracted: 90000 [PASS]
Original PTS: 450000 -> Encoded -> Extracted: 450000 [PASS]
Original PTS: 8589934591 -> Encoded -> Extracted: 8589934591 [PASS]

=== Testing PTS Discontinuity Correction ===
Test 1 - Normal progression:
  PTS1: 90000 (1000ms)
  PTS2: 180000 (2000ms)
  Delta: 90000 (1000ms)
  Discontinuity detected: NO

Test 2 - Large jump (discontinuity):
  PTS1: 90000 (1000ms)
  PTS2: 9000000 (100000ms)
  Delta: 8910000 (99000ms)
  Discontinuity detected: YES
  Correction offset: -8910000 (-99000ms)
  Corrected PTS2: 90000 (1000ms)

Test 3 - Backward jump:
  PTS1: 9000000 (100000ms)
  PTS2: 90000 (1000ms)
  Delta: -8910000 (-99000ms)
  Discontinuity detected: YES

=== Test Results ===
PTS Encoding/Decoding: PASS
Discontinuity Detection: PASS

All tests PASSED! PTS discontinuity correction logic is working correctly.
```

## Integration in Tardsplaya

The PTS discontinuity fix is automatically enabled when using transport stream mode (default). Key features:

### Automatic Detection
- Monitors PTS values in video and audio streams
- Detects jumps exceeding 20 second threshold
- Handles both forward and backward timestamp jumps

### Seamless Correction
- Applies offset corrections to maintain smooth playback
- Works transparently with existing buffering and frame tagging
- Resets state automatically on SCTE-35 discontinuities

### Configuration
```cpp
RouterConfig config;
config.enable_pts_discontinuity_correction = true;  // Enable (default)
config.discontinuity_threshold_ms = 20000;          // 20 second threshold
```

## Common Scenarios

### HLS Ad Breaks
- **Problem**: SCTE-35 ad insertions cause timestamp jumps
- **Solution**: Automatically detects and corrects PTS offsets
- **Result**: Smooth transition back to main content

### Stream Switching
- **Problem**: Quality changes may have different timestamp bases
- **Solution**: Maintains continuous timeline across switches
- **Result**: No audio/video sync issues

### Broken Encoders
- **Problem**: Some encoders produce inconsistent timestamps
- **Solution**: Normalizes timestamps for consistent playback
- **Result**: Stable playback on problematic streams

## Debugging

Enable debug logging to monitor PTS correction:

```
[PTS_DISCONTINUITY] Video PTS jump detected: 99000ms, applying offset: -99000ms
[PTS_CORRECTION] Enabled with threshold: 20000ms
[DISCONTINUITY] Detected ad transition - implementing fast restart
```

The fix automatically logs when discontinuities are detected and corrected, making it easy to troubleshoot timestamp issues.