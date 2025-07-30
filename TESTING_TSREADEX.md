# TSReadEX Integration Test

This document describes how to test the TSReadEX integration in Tardsplaya.

## What Was Implemented

The TSReadEX integration provides optional MPEG-TS stream enhancement with the following capabilities:

1. **Stream Filtering**: Removes unwanted data like EIT (program guide) packets
2. **PID Standardization**: Remaps packet IDs to standard values for better compatibility
3. **Audio Enhancement**: Ensures consistent audio streams and handles missing audio
4. **Stream Stabilization**: Processes transport streams to handle edge cases

## How to Test

### 1. Enable TSReadEX in Settings
1. Launch Tardsplaya
2. Go to **Tools → Settings**
3. In the "TSReadEX Stream Enhancement" section:
   - ✅ Check "Enable TSReadEX processing"
   - ✅ Check "Remove program guide data" (recommended)
   - ✅ Check "Stabilize audio streams" (recommended)
4. Click **OK**

### 2. Test Streaming with TSReadEX
1. Enter a Twitch channel name
2. Click **Load** to fetch qualities
3. Select a quality and click **Watch**
4. Observe the log messages:
   - Should show **[TS-ROUTER]** messages instead of **[TX-QUEUE]**
   - Look for **"TSReadEX processor initialized"** message
   - During streaming, watch for **[TSREADEX]** processing messages

### 3. Compare Performance
Test both modes to compare:

**Without TSReadEX** (TX-Queue mode):
- Disable TSReadEX in settings
- Start a stream - should show **[TX-QUEUE]** messages
- Note buffering behavior and player compatibility

**With TSReadEX** (Transport Stream mode):
- Enable TSReadEX in settings  
- Start the same stream - should show **[TS-ROUTER]** messages
- Compare stream quality and player compatibility

## Expected Behavior

### With TSReadEX Enabled:
- Streaming mode: **Transport Stream** with TSReadEX processing
- Log messages indicate **[TS-ROUTER]** and **[TSREADEX]** activity
- Potentially cleaner stream data sent to media player
- Better compatibility with picky media players

### With TSReadEX Disabled:
- Streaming mode: **TX-Queue IPC** (high-performance default)
- Log messages show **[TX-QUEUE]** activity
- Direct segment streaming without additional processing

## Troubleshooting

If TSReadEX causes issues:
1. Disable it in Settings to return to TX-Queue mode
2. Check logs for **[TSREADEX]** error messages
3. Verify media player supports transport stream input
4. Some unusual stream formats may not be compatible

## Benefits Analysis

TSReadEX is most beneficial when:
- Using media players that are sensitive to stream format variations
- Experiencing audio/video sync issues with certain streams
- Wanting cleaner streams with less unnecessary data
- Need standardized PID assignments for better compatibility

The integration maintains full backward compatibility - disabling TSReadEX returns to the original high-performance TX-Queue streaming mode.