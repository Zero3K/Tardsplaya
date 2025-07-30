# Demux-MPEGTS Configuration Guide

This guide explains how to configure and use the Demux-MPEGTS mode in Tardsplaya for better discontinuity recovery.

## Overview

Demux-MPEGTS mode separates MPEG transport streams into independent video and audio elementary streams, feeding them to separate media players. This prevents the common issue where media players freeze on a black screen or last video frame while audio continues playing after stream discontinuities.

## Media Player Requirements

### Recommended: MPV
```
Player Path: mpv.exe
Video Args: --video-only --no-audio --
Audio Args: --audio-only --no-video --
```

### Alternative: VLC
```
Player Path: vlc.exe
Video Args: --intf dummy --no-audio --
Audio Args: --intf dummy --no-video --
```

### Alternative: MPC-HC/MPC-BE
```
Player Path: mpc-hc64.exe
Video Args: /audioonly false /close /minimized /play
Audio Args: /videoonly false /close /minimized /play
```

## Configuration Options

### Default Configuration (Automatic)
Tardsplaya now uses Demux-MPEGTS mode by default. No configuration changes are needed for basic operation.

### Advanced Configuration
For advanced users, you can modify the configuration in the source code:

```cpp
// In demux_mpegts_wrapper.h, modify DemuxConfig struct:
struct DemuxConfig {
    // Player settings
    std::wstring player_path = L"mpv.exe";
    std::wstring video_player_args = L"--video-only --no-audio --";
    std::wstring audio_player_args = L"--audio-only --no-video --";
    
    // Stream behavior
    bool enable_separate_streams = true;      // Main feature toggle
    bool enable_stream_recovery = true;       // Auto-recovery from discontinuities
    bool enable_packet_buffering = true;      // Buffer packets for smooth playback
    
    // Buffer sizes (adjust for performance vs latency)
    size_t max_video_buffer_packets = 1000;   // ~5-10 seconds of video
    size_t max_audio_buffer_packets = 2000;   // ~10-20 seconds of audio
    
    // Recovery settings
    std::chrono::milliseconds stream_timeout{10000};  // Stream considered dead after 10s
    uint32_t max_consecutive_errors = 10;              // Max errors before stream reset
    bool auto_restart_streams = true;                  // Auto-restart failed streams
    
    // Debug settings
    bool enable_debug_logging = false;        // Enable detailed demux logging
};
```

## How It Works

1. **Stream Download**: Downloads HLS segments containing MPEG transport streams
2. **Demuxing**: Uses demux-mpegts library to separate video and audio elementary streams
3. **Dual Players**: Launches separate video-only and audio-only media players
4. **Health Monitoring**: Continuously monitors both streams for errors and discontinuities
5. **Automatic Recovery**: Restarts failed streams and recovers from discontinuities automatically

## Benefits

### Before Demux-MPEGTS
- Media player receives combined MPEG-TS stream
- Discontinuities cause player to freeze on last video frame
- Audio continues playing while video is stuck
- Manual restart required to recover

### After Demux-MPEGTS
- Separate video and audio streams sent to independent players
- Video stream can recover independently from audio stream
- Automatic detection and recovery from discontinuities
- No manual intervention required for most recovery scenarios

## Troubleshooting

### Issue: Players not launching
**Solution**: Ensure media player is in PATH or provide full path:
```cpp
config.player_path = L"C:\\Program Files\\mpv\\mpv.exe";
```

### Issue: High CPU usage
**Solution**: Reduce buffer sizes:
```cpp
config.max_video_buffer_packets = 500;  // Reduce from 1000
config.max_audio_buffer_packets = 1000; // Reduce from 2000
```

### Issue: Stream lag
**Solution**: Enable low-latency mode (modify source):
```cpp
// In DemuxMpegtsWrapper constructor
config_.buffer_timeout = std::chrono::milliseconds(1000); // Reduce from 5000
```

### Issue: Frequent restarts
**Solution**: Increase error tolerance:
```cpp
config.max_consecutive_errors = 20;     // Increase from 10
config.stream_timeout = std::chrono::milliseconds(20000); // Increase from 10000
```

## Performance Recommendations

### For Low-End Systems
- Reduce buffer sizes
- Disable debug logging
- Use lightweight media player (mpv recommended)

### For High-End Systems
- Increase buffer sizes for smoother playback
- Enable debug logging for troubleshooting
- Use hardware-accelerated media players

### For Multiple Streams
- Consider switching to TX-Queue mode for better multi-stream performance
- Or reduce buffer sizes when running multiple demux streams simultaneously

## Debug Information

Enable debug logging to see detailed demux information:
```cpp
config.enable_debug_logging = true;
```

This will show:
- Stream detection and format information
- Packet processing statistics
- Error details and recovery attempts
- Player process status
- Buffer utilization

## Switching Between Modes

To switch back to other streaming modes, modify `Tardsplaya.cpp`:

```cpp
// For TX-Queue mode (high performance)
StreamingMode mode = StreamingMode::TX_QUEUE_IPC;

// For Transport Stream mode (professional)
StreamingMode mode = StreamingMode::TRANSPORT_STREAM;

// For traditional HLS mode (compatibility)
StreamingMode mode = StreamingMode::HLS_SEGMENTS;

// For Demux-MPEGTS mode (discontinuity recovery) - Default
StreamingMode mode = StreamingMode::DEMUX_MPEGTS_STREAMS;
```