# PTS Discontinuity Fix Implementation

This implementation adds PTS (Presentation Timestamp) discontinuity correction to Tardsplaya's transport stream router, based on the FFmpeg HLS PTS discontinuity reclocking functionality.

## Problem

Broken bitstreams with timestamp offsets (especially common in HLS streams with SCTE-35 discontinuities) can cause:
- Audio/video synchronization issues
- Player buffering problems
- Stuttering during ad breaks
- Inconsistent playback timing

## Solution

The implementation adds timestamp discontinuity detection and correction directly into the HLS-to-TS conversion process:

### Key Components

1. **TSPacket PTS/DTS Parsing** (`tsduck_transport_router.cpp`):
   - `ParsePTSDTS()`: Extracts PTS/DTS from PES headers
   - `ApplyTimestampCorrection()`: Modifies PTS/DTS values in-place

2. **HLSToTSConverter Discontinuity Detection**:
   - `CheckAndCorrectDiscontinuity()`: Monitors for timestamp jumps
   - Configurable threshold (default: 20 seconds)
   - Separate tracking for video and audio streams

3. **RouterConfig Options**:
   - `enable_pts_discontinuity_correction`: Enable/disable correction (default: true)
   - `discontinuity_threshold_ms`: Threshold for detecting jumps (default: 20000ms)

### How It Works

1. **Detection**: Monitors PTS values between consecutive packets
2. **Threshold Check**: Compares timestamp delta against configurable threshold
3. **Offset Calculation**: Calculates correction offset to maintain continuity
4. **Application**: Modifies PTS/DTS values in transport stream packets
5. **Reset on SCTE-35**: Resets correction state when playlist discontinuities are detected

### Integration

- Enabled by default in `StreamingMode::TRANSPORT_STREAM`
- Automatically resets on SCTE-35 discontinuity markers
- Works seamlessly with existing frame number tagging and buffering

### Testing

The implementation includes comprehensive tests:
- PTS encoding/decoding validation
- Discontinuity detection logic
- Correction offset calculations
- Edge case handling (backward jumps, wraparound)

### Configuration

Users can configure the discontinuity correction via `RouterConfig`:

```cpp
RouterConfig config;
config.enable_pts_discontinuity_correction = true;  // Enable correction
config.discontinuity_threshold_ms = 20000;          // 20 second threshold
```

This fix ensures smooth playback across HLS discontinuities while maintaining compatibility with the existing codebase.