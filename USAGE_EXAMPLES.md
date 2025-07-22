# HLS PTS Discontinuity Reclock - Usage Examples

This document shows how to use the HLS PTS discontinuity reclock tool that was created for Tardsplaya.

## Automatic Integration (Default)

When using Tardsplaya normally, the PTS correction happens automatically:

1. **Start Tardsplaya** - Open the application as usual
2. **Enter Stream URL** - Use any HLS stream (like Twitch channels)
3. **Auto-Detection** - System detects if stream needs PTS correction
4. **Seamless Correction** - If needed, applies correction transparently
5. **Smooth Playback** - Media player receives corrected stream

No user intervention required - it just works better!

## Standalone Tool Usage

The `hls-pts-reclock.exe` tool can also be used independently:

### Basic Usage

```bash
# Basic correction from HLS to MPEG-TS
hls-pts-reclock.exe input.m3u8 output.ts

# Using explicit parameters
hls-pts-reclock.exe -i http://stream.example.com/playlist.m3u8 -o corrected.ts

# Output to FLV format for RTMP
hls-pts-reclock.exe -f flv input.m3u8 output.flv
```

### Advanced Options

```bash
# Verbose output with custom thresholds
hls-pts-reclock.exe --verbose \
  --threshold 500000 \
  --delta-threshold 5.0 \
  http://live.stream.com/index.m3u8 \
  corrected_stream.ts

# Disable monotonicity correction (for testing)
hls-pts-reclock.exe --no-monotonicity input.m3u8 output.ts

# Debug mode with detailed logging
hls-pts-reclock.exe --debug -v input.m3u8 output.ts
```

## Example Scenarios

### Scenario 1: Twitch Stream with Ads

```bash
# Twitch streams often have discontinuities during ad breaks
hls-pts-reclock.exe https://usher.ttvnw.net/api/channel/hls/streamer.m3u8 twitch_corrected.ts

# Expected output:
# HLS PTS Discontinuity Reclock Tool
# ==================================
# 
# Processing HLS stream: https://usher.ttvnw.net/api/channel/hls/streamer.m3u8
# Output: twitch_corrected.ts (format: mpegts)
# Starting HLS processing...
# 
# Packet 0: Normal PTS/DTS
# Packet 25: ** DISCONTINUITY DETECTED ** (ad break)
# Packet 26: Applied correction offset: +2.5s
# Packet 45: ** DISCONTINUITY DETECTED ** (return from ad)
# Packet 46: Applied correction offset: +0.8s
# 
# Processing complete. Statistics:
#   Total packets processed: 127
#   Discontinuities detected: 4
#   Timestamp corrections applied: 52
#   Total offset applied: 3.847s
```

### Scenario 2: Live News Stream

```bash
# Live streams with frequent switches between sources
hls-pts-reclock.exe --threshold 1000000 \
  http://news.stream.com/live.m3u8 \
  news_smooth.ts

# Results in smooth playback without stuttering during source switches
```

### Scenario 3: Converting to RTMP

```bash
# Convert HLS with discontinuities to smooth RTMP stream
hls-pts-reclock.exe -f flv \
  http://source.stream.com/index.m3u8 \
  smooth_for_rtmp.flv

# Then stream to RTMP server:
# ffmpeg -re -i smooth_for_rtmp.flv -c copy -f flv rtmp://server.com/live/stream
```

## Integration in Other Applications

The standalone tool can be integrated into other streaming applications:

### Python Integration

```python
import subprocess
import os

def process_hls_with_pts_correction(input_url, output_path):
    cmd = [
        "hls-pts-reclock.exe",
        "-i", input_url,
        "-o", output_path,
        "--verbose"
    ]
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode == 0:
        print(f"Successfully corrected stream: {output_path}")
        return output_path
    else:
        print(f"Correction failed: {result.stderr}")
        return input_url  # Fallback to original
```

### Batch Processing

```bash
#!/bin/bash
# Process multiple streams with PTS correction

streams=(
    "http://stream1.com/index.m3u8"
    "http://stream2.com/playlist.m3u8" 
    "http://stream3.com/live.m3u8"
)

for i in "${!streams[@]}"; do
    echo "Processing stream $((i+1))/${#streams[@]}: ${streams[$i]}"
    
    hls-pts-reclock.exe \
        --verbose \
        "${streams[$i]}" \
        "corrected_stream_$i.ts"
        
    if [ $? -eq 0 ]; then
        echo "✓ Stream $i corrected successfully"
    else
        echo "✗ Stream $i correction failed"
    fi
done
```

## Configuration Files

Create a config file for consistent settings:

```ini
# hls-reclock.conf
[general]
force_monotonicity=true
verbose=false

[thresholds]
discontinuity_threshold=1000000
delta_threshold=10.0
error_threshold=108000.0

[output]
default_format=mpegts
temp_directory=temp_reclock
```

## Monitoring and Logging

The tool provides detailed logging for monitoring:

```bash
# Enable full logging
hls-pts-reclock.exe --debug --verbose \
  input.m3u8 output.ts 2>&1 | tee reclock.log

# Log analysis
grep "DISCONTINUITY" reclock.log
grep "correction" reclock.log
grep "Statistics" reclock.log
```

## Performance Considerations

- **CPU Usage**: Minimal overhead, mostly I/O bound
- **Memory Usage**: Processes streams in chunks, low memory footprint  
- **Latency**: Adds ~1-2 seconds for processing (acceptable for most use cases)
- **Throughput**: Can handle multiple streams simultaneously

## Troubleshooting

### Common Issues

1. **Tool not found**: Ensure `hls-pts-reclock.exe` is in same directory as Tardsplaya
2. **Permission errors**: Run as administrator if accessing protected directories
3. **Network timeouts**: Use longer timeout values for slow connections
4. **Large corrections**: May indicate severely corrupted stream

### Debug Mode

```bash
# Maximum debugging information
hls-pts-reclock.exe --debug --verbose \
  --threshold 100000 \
  problematic_stream.m3u8 \
  debug_output.ts
```

This will show detailed information about every timestamp decision made during processing.

## Summary

The HLS PTS discontinuity reclock tool provides:

- ✅ **Automatic correction** of timestamp discontinuities
- ✅ **Professional-grade** output suitable for broadcast
- ✅ **Multiple output formats** (MPEG-TS, FLV)
- ✅ **Configurable thresholds** for different stream types
- ✅ **Standalone operation** for integration with other tools
- ✅ **Comprehensive logging** for monitoring and debugging

This makes problematic HLS streams work smoothly with any media player or streaming system.