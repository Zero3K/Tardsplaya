# Tardsplaya

A Twitch stream player for Windows with enhanced TLS support.

## Features

- Multi-tab stream viewing
- Quality selection
- Windows 7+ compatibility
- **Enhanced TLS Client Support** - Custom TLS implementation for better compatibility with older Windows versions

## TSDuck Integration for Enhanced Performance

This version includes **TSDuck-inspired HLS processing** for improved streaming performance and reduced lag.

### Performance Enhancements

- **Advanced HLS Parsing**: TSDuck-inspired playlist analysis with precise timing calculations
- **Smart Buffering**: Dynamic buffer sizing based on stream characteristics
- **Enhanced Ad Detection**: Sophisticated SCTE-35 and pattern-based ad detection
- **Optimized Timing**: Better segment timing for smoother playback

### NEW: Frame Number Tagging for Lag Reduction

**Frame Number Tagging** - Advanced frame tracking and monitoring system:

- **Frame Sequence Tracking**: Each transport stream packet receives unique frame numbers for precise ordering
- **Drop Detection**: Automatically detects and reports dropped frames that can cause lag
- **Duplicate Detection**: Identifies duplicate or reordered frames that may indicate network issues  
- **Key Frame Identification**: Marks I-frames and key frames for better buffering decisions
- **Real-time Statistics**: Displays current FPS, frame drops, and timing information
- **Lag Analysis**: Provides detailed frame timing data to help identify lag sources

### Technical Details

Frame Number Tagging enhances the existing TSDuck transport stream system:

- **TSPacket Enhancement**: Added frame numbering, timing, and metadata to each packet
- **HLS Converter Integration**: Assigns frame numbers during segment conversion
- **Statistical Monitoring**: Tracks frame rates, drops, and timing across all streams
- **Debug Logging**: Detailed frame information for troubleshooting lag issues
- **Status Display**: Real-time frame statistics in the status bar

- **Stream Re-routing**: Routes HLS streams through transport stream format to media players
- **Built-in by Default**: TSDuck TS Mode is now the standard streaming method
- **Smart Buffering**: Packet-level buffering (~5000 packets default, ~940KB) for smoother playback
- **PAT/PMT Generation**: Proper MPEG-TS structure with Program Association and Program Map tables
- **PCR Insertion**: Program Clock Reference timing for better synchronization

### Technical Details

The TSDuck integration includes:

- `tsduck_hls_wrapper.h/cpp` - Lightweight TSDuck-inspired HLS parser
- `tsduck_transport_router.h/cpp` - **NEW** Transport stream re-routing engine
- Enhanced playlist analysis with precise duration calculations  
- Dynamic buffer optimization based on stream characteristics
- Advanced ad detection using multiple detection patterns
- SCTE-35 processing for professional-grade ad handling

### Performance Benefits

| Feature | Before | After |
|---------|--------|--------|
| Buffer Management | Static 3 segments | Dynamic 2-8 segments based on content |
| Ad Detection | Basic pattern matching | TSDuck-style multi-pattern analysis |
| Timing Precision | Basic duration parsing | Millisecond-precise calculations |
| Lag Reduction | Manual tuning | Automatic optimization + Frame Number Tagging |
| **Frame Tracking** | **No frame monitoring** | **Real-time frame numbering and drop detection** |
| **Lag Analysis** | **Basic logging only** | **Detailed frame statistics and timing data** |
| **Stream Format** | **HLS segments only** | **HLS segments + Transport Stream routing** |
| **Player Compatibility** | **Basic stdin piping** | **Professional TS format support** |

The integration works transparently:
1. **Primary**: TSDuck-enhanced parsing for optimal performance
2. **Fallback**: Original parsing if TSDuck analysis fails
3. **Dynamic**: Buffer sizes adjust automatically based on content
4. **NEW**: Optional transport stream re-routing for professional media players

### TSDuck Transport Stream Mode (Default)

**Default Streaming Mode**: TSDuck TS Mode is now the standard streaming method:

- **Automatic Conversion**: HLS segments → MPEG Transport Stream packets
- **Professional Format**: Standard broadcast-quality TS format
- **Enhanced Buffering**: Packet-level buffering instead of segment buffering  
- **Better Compatibility**: Works with professional media players expecting TS format
- **Reduced Latency**: Continuous stream instead of segment-based delivery

TSDuck TS Mode provides superior performance and compatibility compared to traditional HLS segment streaming.

## TLS Client Integration

This version includes an integrated TLS client from the [tlsclient](https://github.com/zero3k/tlsclient) repository, providing:

- **TLS 1.2/1.3 Support**: Modern encryption standards
- **Windows 7 Compatibility**: Better HTTPS support on older Windows versions
- **Fallback Mechanism**: Automatically switches to TLS client when WinHTTP fails
- **Certificate Bypass**: Designed for compatibility without strict certificate validation

### Technical Details

The TLS client integration includes:

- `tlsclient.h/cpp` - Main TLS client wrapper
- Crypto components: `chacha20.c`, `ecc.c`, `gcm.c`, `sha2.c`
- TLS protocol implementation: `tls.h`, `tlsclient_source.cpp`
- Thread-safe locking: `lock.h`

The integration works as a fallback system:
1. Primary: WinHTTP (standard Windows HTTP library)
2. Fallback: Custom TLS client (when WinHTTP fails or on older systems)

## Building

Requires Visual Studio 2015 or later. The project includes all necessary TLS client components and will compile them automatically.

## Media Player Compatibility

### MPC-HC/MPC-BE Workaround

This version includes an automatic workaround for an issue with MPC-HC and MPC-BE where video freezes on black frames after ad skipping while audio continues playing. The workaround is enabled automatically when these players are detected.

**How it works:**
- Detects MPC-HC, MPC-BE, VLC, and PotPlayer automatically
- Forces video synchronization recovery during ad transitions
- Inserts synthetic key frame markers to help players recover
- Ensures proper stream discontinuity signaling

**Supported Players:**
- ✅ MPV (no workaround needed)
- ✅ MPC-HC (workaround enabled automatically)  
- ✅ MPC-BE (workaround enabled automatically)
- ✅ VLC (workaround enabled automatically)
- ✅ PotPlayer (workaround enabled automatically)

The workaround activates automatically when problematic players are detected and operates transparently in the background.

## Compatibility

- Windows 7 SP1 and later
- Enhanced TLS support for older Windows versions
- Automatically handles TLS compatibility issues - Twitch Stream Buffer

A C++ application that buffers Twitch streams to media players like MPC-HC, MPC-BE, VLC, and MPV.

## Features

- **Stream Buffering**: Downloads and buffers stream segments before sending to media player for smooth playback
- **Multiple Quality Support**: Automatically detects and allows selection of available stream qualities
- **Multi-Stream Support**: Open multiple tabs to watch different streams simultaneously
- **Windows 7 Compatibility**: Includes certificate validation workarounds for older Windows versions
- **Real-time Logging**: Shows detailed logs of streaming operations
- **Modern C++ Implementation**: Clean, efficient C++17 code with minimal dependencies

## Requirements

- Windows 7 or later
- Media player (MPV recommended, also supports VLC, MPC-HC, MPC-BE)
- Internet connection

## Usage

1. **Launch the application**
2. **Enter a Twitch channel name** in the Channel field
3. **Click "Load"** to fetch available stream qualities
4. **Select a quality** from the dropdown list
5. **Click "Watch"** to start buffered streaming
6. **Click "Stop"** to stop the stream

### Multiple Streams

- Use **File → New Stream** (Ctrl+T) to open additional tabs
- Use **File → Close Active Stream** (Ctrl+W) to close the current tab
- Use **File → Close All Streams** to close all tabs

## Configuration

The application defaults to using MPV as the media player. To use a different player:

1. Go to **Tools → Settings** (when implemented)
2. Set the player path and arguments

Default settings:
- Player: `mpv.exe`
- Arguments: `-` (reads from stdin)

## Technical Improvements

This C++ version includes several improvements over the original:

### Windows 7 Compatibility
- Certificate validation bypass for HTTPS requests
- Compatible with older root certificate stores

### Modern API Usage
- Updated Twitch API endpoints
- Proper error handling and retry logic
- Thread-safe logging system

### Memory Efficiency
- Minimal memory footprint
- Efficient stream buffering
- Clean resource management

### Code Quality
- Removed deprecated `<codecvt>` header usage
- Modern C++17 features
- Comprehensive error handling

## Building

Requirements:
- Visual Studio 2019 or later
- Windows SDK 10.0

Build steps:
1. Open `Tardsplaya.sln` in Visual Studio
2. Select Release configuration
3. Build → Build Solution

## License

GNU General Public License v3.0

## Contributing

Feel free to submit issues and pull requests to improve the application.
