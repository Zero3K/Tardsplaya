# Demux-MPEGTS Configuration Guide

This guide explains how to configure and use the Demux-MPEGTS mode in Tardsplaya for better discontinuity recovery.

## Overview

Demux-MPEGTS mode separates MPEG transport streams into independent video and audio elementary streams. Two modes are supported:

1. **Single Player Mode (Recommended)**: Uses one media player with external audio file support
2. **Legacy Separate Players Mode**: Uses separate video-only and audio-only player instances

Both modes prevent the common issue where media players freeze on a black screen or last video frame while audio continues playing after stream discontinuities.

## Single Player Mode (Recommended)

This mode uses a single media player with external audio file support, providing better synchronization and compatibility.

### Supported Media Players

#### MPV (Recommended)
```
Player Path: mpv.exe
Command Format: mpv.exe --audio-file=audio.aac video.h264
```

#### MPC-HC / MPC-BE
```
Player Path: mpc-hc64.exe
Command Format: mpc-hc64.exe /dub audio.aac video.h264
```

#### VLC Media Player
```
Player Path: vlc.exe
Command Format: vlc.exe --input-slave=audio.aac video.h264
```

### Benefits of Single Player Mode
- Better audio/video synchronization
- Single player process (lower resource usage)
- More compatible with player features (seeking, controls, etc.)
- Automatic discontinuity recovery

## Legacy Separate Players Mode

For compatibility with older configurations or specific requirements.

### MPV (Separate Players)
```
Player Path: mpv.exe
Video Args: --video-only --no-audio --
Audio Args: --audio-only --no-video --
```

### VLC (Separate Players)
```
Player Path: vlc.exe
Video Args: --intf dummy --no-audio --
Audio Args: --intf dummy --no-video --
```

### MPC-HC/MPC-BE (Separate Players)
```
Player Path: mpc-hc64.exe
Video Args: /audioonly false /close /minimized /play
Audio Args: /videoonly false /close /minimized /play
```

## Configuration Options

### Default Configuration (Automatic)
Tardsplaya now uses Demux-MPEGTS mode with single player mode by default. No configuration changes are needed for basic operation.

### Advanced Configuration
For advanced users, you can modify the configuration in the source code:

```cpp
// In demux_mpegts_wrapper.h, modify DemuxConfig struct:
struct DemuxConfig {
    // Player settings
    std::wstring player_path = L"mpv.exe";
    MediaPlayerType player_type = MediaPlayerType::MPV;
    
    // Mode selection
    bool use_single_player_mode = true;       // Recommended: single player with external audio
    std::wstring video_file_extension = L".h264";
    std::wstring audio_file_extension = L".aac";
    
    // Legacy separate players mode
    std::wstring video_player_args = L"--video-only --no-audio --";
    std::wstring audio_player_args = L"--audio-only --no-video --";
    
    // Stream behavior
    bool enable_separate_streams = true;      // Main feature toggle
    bool enable_stream_recovery = true;       // Auto-recovery from discontinuities
    bool enable_packet_buffering = true;      // Buffer packets for smooth playback
    
    // Buffer sizes (adjust for performance vs latency)
    size_t max_video_buffer_packets = 1000;   // ~5-10 seconds of video
    size_t max_audio_buffer_packets = 2000;   // ~10-20 seconds of audio
    
    // File management for single player mode
    std::chrono::milliseconds file_buffer_duration{5000}; // Buffer before starting player
    size_t max_file_size_mb = 100;                        // Max temp file size (MB)
    bool cleanup_temp_files = true;                       // Clean up temp files on exit
    std::wstring temp_directory;                          // Custom temp dir (empty = system temp)
    
    // Recovery settings
    std::chrono::milliseconds stream_timeout{10000};  // Stream considered dead after 10s
    uint32_t max_consecutive_errors = 10;              // Max errors before stream reset
    bool auto_restart_streams = true;                  // Auto-restart failed streams
    
    // Debug settings
    bool enable_debug_logging = false;        // Enable detailed demux logging
};
```

## How It Works

### Single Player Mode (Default)
1. **Stream Download**: Downloads HLS segments containing MPEG transport streams
2. **Demuxing**: Uses demux-mpegts library to separate video and audio elementary streams
3. **File Creation**: Writes video and audio streams to temporary files
4. **Single Player Launch**: Launches one media player with external audio file parameter
5. **Health Monitoring**: Continuously monitors stream and player for errors and discontinuities
6. **Automatic Recovery**: Restarts player and recovers from discontinuities automatically

### Legacy Separate Players Mode
1. **Stream Download**: Downloads HLS segments containing MPEG transport streams
2. **Demuxing**: Uses demux-mpegts library to separate video and audio elementary streams
3. **Dual Players**: Launches separate video-only and audio-only media players
4. **Health Monitoring**: Continuously monitors both streams for errors and discontinuities
5. **Automatic Recovery**: Restarts failed streams and recovers from discontinuities automatically

## Benefits

### Single Player Mode Benefits
- **Better Synchronization**: Single player naturally keeps audio and video synchronized
- **Lower Resource Usage**: Only one player process instead of two
- **Player Features**: Full access to player features like seeking, controls, etc.
- **Compatibility**: Works with player-specific audio file features

### General Demux-MPEGTS Benefits

#### Before Demux-MPEGTS
- Media player receives combined MPEG-TS stream
- Discontinuities cause player to freeze on last video frame
- Audio continues playing while video is stuck
- Manual restart required to recover

#### After Demux-MPEGTS
- Separate video and audio streams (either to files or separate players)
- Independent recovery capability for video and audio
- Automatic detection and recovery from discontinuities
- No manual intervention required for most recovery scenarios

## Troubleshooting

### Issue: Player not launching (Single Player Mode)
**Solution**: Ensure media player supports external audio files and is in PATH:
```cpp
config.player_path = L"C:\\Program Files\\mpv\\mpv.exe";
config.player_type = MediaPlayerType::MPV;
```

### Issue: Players not launching (Separate Players Mode)
**Solution**: Ensure media player is in PATH or provide full path:
```cpp
config.player_path = L"C:\\Program Files\\mpv\\mpv.exe";
config.use_single_player_mode = false;  // Use legacy mode
```

### Issue: Temporary files not cleaned up
**Solution**: Enable cleanup and check temp directory:
```cpp
config.cleanup_temp_files = true;
config.temp_directory = L"C:\\Temp\\Tardsplaya\\";  // Custom temp directory
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