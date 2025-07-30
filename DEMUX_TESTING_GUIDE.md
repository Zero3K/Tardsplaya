# Demux MPEG-TS Integration Test Guide

## Overview

This implementation adds MPEG-TS demuxing capability to Tardsplaya using the demux-mpegts library by janbar. The demux functionality separates video and audio streams into individual files and launches the media player with appropriate command line arguments.

## Features Added

### 1. MPEG-TS Demux Streaming Mode
- New `DEMUX_MPEGTS` streaming mode
- Separates video and audio into individual elementary stream files
- Supports multiple media players with different command line formats

### 2. Media Player Support
- **MPC-HC/MPC-BE**: Uses `/dub filename` parameter for additional audio
- **VLC**: Uses `--input-slave=filename` parameter for additional audio
- **MPV**: Uses `--audio-file=filename` parameter for additional audio

### 3. UI Integration
- Added streaming mode selection in Settings dialog
- Modes available:
  - HLS Segments (Fallback)
  - Transport Stream (TSDuck)
  - TX-Queue IPC (High Performance) - Default
  - MPEG-TS Demux (Separate A/V) - New

## Testing Instructions

### Prerequisites
1. Build the application with demux-mpegts integration
2. Have a compatible media player installed (MPV, VLC, or MPC-HC)
3. Configure the media player path in Settings

### Test Steps

#### 1. Basic Demux Functionality Test
1. Launch Tardsplaya
2. Go to Tools → Settings
3. Select "MPEG-TS Demux (Separate A/V)" from Streaming Mode dropdown
4. Click OK to save settings
5. Enter a Twitch channel name and click Load
6. Select a quality and click Watch
7. Verify:
   - Log shows "[DEMUX] Starting MPEG-TS demux streaming"
   - Separate video and audio files are created in temp directory
   - Media player launches with both files

#### 2. Media Player Compatibility Test
Test with different media players:

**MPV Test:**
- Set Player Path to `mpv.exe`
- Expected command: `mpv.exe "video_file.h264" --audio-file="audio_file.aac"`

**VLC Test:**
- Set Player Path to `vlc.exe` 
- Expected command: `vlc.exe "video_file.h264" --input-slave="audio_file.aac"`

**MPC-HC Test:**
- Set Player Path to `mpc-hc.exe` or `mpc-hc64.exe`
- Expected command: `mpc-hc.exe "video_file.h264" /dub "audio_file.aac"`

#### 3. Stream Recovery Test
1. Start streaming with demux mode
2. Wait for playback to begin
3. Simulate network discontinuity (pause/resume internet connection)
4. Verify:
   - Separate files continue to be written
   - Media player can recover playback more smoothly
   - No extended black screen/audio desync issues

#### 4. Multi-Stream Test
1. Open multiple tabs (Ctrl+T)
2. Set different channels to demux mode
3. Verify:
   - Each stream creates separate output directories
   - Multiple media players launch correctly
   - No interference between demux processes

### Expected Output Files

When demux mode is active, files are created in:
```
%TEMP%\Tardsplaya_Demux_<channel>_<timestamp>\
├── stream_<channel>_<video_PID>.h264    # Video elementary stream
├── stream_<channel>_<audio_PID>.aac     # Audio elementary stream
└── (additional streams as detected)
```

### Debugging

Enable verbose debug logging in Settings for detailed demux information:
- Stream detection logs
- File creation logs
- Demux statistics
- Player command line generation

### Common Issues

1. **"Failed to initialize demux system"**
   - Check temp directory permissions
   - Verify demux-mpegts library is included in build

2. **"Media player failed to launch"**
   - Verify player path is correct
   - Check player supports the command line format used

3. **"No streams detected"**
   - Verify the source is valid MPEG-TS format
   - Check if stream contains video/audio elementary streams

4. **Audio/Video desync**
   - Normal behavior - players handle elementary stream synchronization
   - Try different players (MPV typically handles this best)

## Performance Notes

- Demux mode has slightly higher CPU usage than other modes
- Creates temporary files that are cleaned up on application exit
- Memory usage scales with buffer size (default 1MB)
- Best performance with SSD storage for temp files

## Compatibility

- Windows 7+ (matches existing application requirements)
- Visual Studio 2015+ for building
- Compatible with all existing Twitch stream formats
- Works with both live streams and VODs that support MPEG-TS format