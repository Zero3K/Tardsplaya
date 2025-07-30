# TS Demuxer Integration Testing Guide

This document provides instructions for testing the new TS Demuxer functionality in Tardsplaya.

## Building with TS Demuxer Support

The TS Demuxer integration is now part of the main build. All necessary source files are included:

- `ts_demuxer.c/.h` - Core TS demuxing functionality  
- `es_output.c/.h` - Elementary stream output handling
- `ts_demuxer_stream.cpp/.h` - C++ integration and streaming management
- `print_out.h` - Logging and output macros (Windows-compatible)

## Testing the TS Demuxer Mode

### Prerequisites
1. Build Tardsplaya with Visual Studio 2019 or later
2. Have a media player installed (MPV recommended)
3. Access to Twitch streams that experience discontinuities

### Basic Testing Steps

1. **Launch Tardsplaya**
2. **Configure TS Demuxer Mode**:
   - Go to Tools → Settings
   - Set "Streaming Mode" to "TS Demuxer (Video/Audio Separation)"
   - Click OK
3. **Test with a Stream**:
   - Enter a channel name
   - Click "1. Load" to fetch qualities
   - Select a quality and click "2. Watch"
   - Monitor the log for TS Demuxer messages

### Expected Log Messages

When TS Demuxer mode is active, you should see log messages like:
```
[TS Demuxer (Video/Audio Separation)] Starting streaming for [channel] (quality)
[TS_DEMUX] Initializing TS Demuxer stream manager for [channel]
[TS_DEMUX] TS Demuxer streaming active for [channel]
[TS_DEMUX] Segments: X, Video: Y, Audio: Z, Bytes: NNNkB
```

### Testing Discontinuity Recovery

To test the discontinuity recovery functionality:

1. **Find a problematic stream** that frequently has discontinuities
2. **Compare behavior** between streaming modes:
   - Test with "TX-Queue IPC" mode first (baseline)
   - Switch to "TS Demuxer" mode and test the same stream
   - Look for differences in player behavior during discontinuities

### Debugging Issues

If the TS Demuxer mode isn't working properly:

1. **Enable verbose logging**:
   - Tools → Settings → Check "Verbose debug"
   - Check "Log to file" to save logs to `debug.log`

2. **Check for common issues**:
   - Player process fails to start
   - TS parsing errors (invalid packet sizes, sync loss)
   - PAT/PMT parsing failures
   - Video/Audio PID detection issues

3. **Compare with other modes**:
   - If TS Demuxer fails, try Transport Stream mode
   - If all modes fail, check player path and arguments

### Performance Monitoring

Monitor the status bar for:
- Segment processing count
- Buffer status
- Mode-specific indicators

The TS Demuxer mode should show:
`Buffer: X packets | TS Demuxer (Video/Audio Separation) Active`

### Advanced Testing

For developers testing the implementation:

1. **Memory leak testing**: Run streams for extended periods
2. **Multiple stream testing**: Open multiple tabs with TS Demuxer mode
3. **Error condition testing**: Test with invalid URLs, network interruptions
4. **Stream format testing**: Test with different types of HLS streams

### Known Limitations

Current implementation limitations:
- Single pipe output (video and audio are interleaved, not separate pipes)
- H.264 video and ADTS AAC audio support primarily
- Memory-based processing (no file I/O like original ts_demuxer)

### Future Enhancements

Planned improvements:
- Separate named pipes for video and audio streams
- Support for additional codecs (H.265, other audio formats)
- Better error recovery and stream format detection
- Performance optimizations for high-bitrate streams

## Troubleshooting

### Common Issues

1. **"Failed to initialize TS Demuxer system"**
   - Check player path in settings
   - Verify media player is accessible

2. **"Failed to start TS Demuxer streaming"**
   - Check network connectivity
   - Verify channel is online and accessible

3. **No video/audio PID detected**
   - Stream may not contain standard H.264/AAC
   - Try switching to Transport Stream mode as fallback

4. **Player process dies immediately**
   - Check player arguments
   - Verify player supports stdin input
   - Try different media player (VLC, MPC-HC, etc.)

### Debug Information

Key debug messages to look for:
- PAT table parsing success/failure
- PMT table parsing and PID detection
- Video/Audio packet counts
- Player process status
- Memory allocation and cleanup

Report issues with detailed logs from verbose debug mode enabled.