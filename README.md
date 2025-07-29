# Tardsplaya

A Twitch stream player for Windows with enhanced TLS support.

## Features

- Multi-tab stream viewing
- Quality selection
- Windows 7+ compatibility
- **Enhanced TLS Client Support** - Custom TLS implementation for better compatibility with older Windows versions
- **GPAC Media Decoding** - Advanced media decoder that converts HLS streams to raw AVI and WAV for optimal player compatibility

## GPAC Media Decoding Integration for Enhanced Performance

**GPAC Decoder Mode** - Advanced media processing system replacing TSDuck:

- **HLS to AVI/WAV Conversion**: Uses GPAC to decode HLS segments into raw AVI video and WAV audio streams
- **Direct Media Player Piping**: Converted streams are piped directly to media players for optimal compatibility
- **Format Compatibility**: Raw AVI and WAV formats ensure maximum compatibility with MPV, VLC, MPC-HC, and other players
- **Real-time Decoding**: Low-latency decoding pipeline for live streaming with minimal delay
- **Comprehensive Statistics**: Built-in monitoring of decoder performance, frame rates, and stream health
- **Adaptive Quality**: Automatic detection and handling of different video/audio qualities and formats

### GPAC Technical Details

The GPAC integration includes:

- `gpac_decoder.h/cpp` - Core GPAC decoder implementation with HLS to AVI/WAV conversion
- Enhanced segment processing with GPAC's multimedia framework
- Real-time statistics and performance monitoring
- Adaptive buffer sizing based on stream characteristics
- Comprehensive error handling and recovery

### Performance Benefits

| Feature | TSDuck (Previous) | GPAC Decoder (Current) |
|---------|-------------------|-------------------------|
| Output Format | MPEG-TS packets | Raw AVI/WAV streams |
| Player Compatibility | Good | Excellent |
| Format Support | Transport streams only | Multiple container formats |
| Decoding Quality | Pass-through | Full decode/re-encode |
| **Media Player Support** | **Limited to TS-capable players** | **Universal media player support** |
| **Quality Control** | **Limited** | **Full control over output quality** |
| **Format Flexibility** | **TS only** | **AVI, WAV, and extensible** |

The integration works in multiple modes:
1. **Primary**: GPAC Decoder mode for optimal media compatibility (default)
2. **Fallback**: TX-Queue IPC mode for high-performance scenarios
3. **Legacy**: TSDuck transport stream mode for professional compatibility
4. **Basic**: Traditional HLS segment streaming for maximum compatibility

### GPAC Decoder Mode (Default)

**Default Streaming Mode**: GPAC Decoder Mode is now the standard streaming method:

- **HLS Segment Decoding**: Processes HLS segments through GPAC's multimedia framework
- **Raw Format Output**: Converts to raw AVI (video) and WAV (audio) for universal player support
- **Direct Piping**: Streams decoded content directly to media player stdin
- **Quality Optimization**: Full control over output quality and format parameters
- **Real-time Processing**: Low-latency decoding pipeline optimized for live streaming

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
