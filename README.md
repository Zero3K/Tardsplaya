# Tardsplaya

A Twitch stream player for Windows with enhanced TLS support.

## Features

- Multi-tab stream viewing
- Quality selection
- Windows 7+ compatibility
- **Enhanced TLS Client Support** - Custom TLS implementation for better compatibility with older Windows versions

## Demux-MPEGTS Integration for Enhanced Discontinuity Recovery

**Demux-MPEGTS Integration** - Advanced MPEG transport stream demultiplexing for better stream reliability:

- **Single Player Mode (Recommended)**: Uses one media player with external audio file support for better synchronization
- **Elementary Stream Separation**: Demuxes MPEG-TS into separate video and audio elementary streams  
- **Independent Stream Recovery**: Video and audio streams can recover independently from discontinuities
- **Black Screen Prevention**: Prevents media players from freezing on last frame when discontinuities occur
- **Automatic Player Restart**: Monitors stream health and automatically restarts failed players
- **Stream Health Monitoring**: Real-time monitoring of video/audio stream integrity with configurable thresholds
- **Multi-Format Support**: Supports H.264, HEVC, AAC, AC3, MPEG audio, and other common stream formats
- **Media Player Compatibility**: Supports MPV (--audio-file), MPC-HC (/dub), VLC (--input-slave), and generic players

### Demux-MPEGTS Technical Details

The Demux-MPEGTS integration includes:

- `demux_mpegts_wrapper.h/cpp` - High-level wrapper for demux-mpegts library integration
- `demux-mpegts/` - Complete demux-mpegts library source code for MPEG-TS parsing
- **Single Player Mode**: Temporary file-based streaming with external audio file support
- **Legacy Mode**: Separate video and audio player process management with automatic restart
- Thread-safe packet queues and file management for smooth video/audio stream delivery
- Comprehensive error handling and recovery mechanisms
- Real-time stream statistics and health monitoring

### Discontinuity Recovery Benefits

| Issue | Before | After |
|-------|--------|--------|
| **Discontinuity Handling** | **Player freezes on black screen** | **Automatic stream recovery** |
| **Audio/Video Sync** | **Audio continues, video frozen** | **Independent stream recovery** |
| **Error Recovery** | **Manual restart required** | **Automatic restart capability** |
| **Stream Monitoring** | **No health monitoring** | **Real-time stream health tracking** |
| **Multi-Format Support** | **Limited codec support** | **Full MPEG-TS format support** |
| **Debugging** | **Limited error information** | **Comprehensive logging and statistics** |

The demux-mpegts integration works seamlessly:
1. **MPEG-TS Input**: Downloads HLS segments containing MPEG transport streams
2. **Stream Demuxing**: Separates video and audio elementary streams using demux-mpegts
3. **Single Player Output (Default)**: Feeds streams to one player with external audio file (e.g., `mpv --audio-file=audio.aac video.h264`)
4. **Health Monitoring**: Continuously monitors streams for discontinuities and errors
5. **Automatic Recovery**: Restarts player and recovers from discontinuities transparently

## TX-Queue IPC Integration for Enhanced Performance

**TX-Queue IPC Mode** - Advanced high-performance inter-process communication system:

- **Lock-Free Queues**: Uses tx-queue's transactional lock-free circular queues for maximum throughput
- **Producer/Consumer Pattern**: Separate threads for downloading segments and feeding to media player
- **Named Pipe Integration**: Maintains compatibility with media players via stdin piping  
- **Checksum Validation**: Built-in data integrity verification for reliable streaming
- **Zero-Copy Operations**: Minimizes memory allocations and data copying
- **Cross-Stream Isolation**: Each stream uses independent tx-queue for optimal multi-stream performance

### TX-Queue Technical Details

The TX-Queue integration includes:

- `tx_queue_ipc.h/cpp` - High-level IPC management with tx-queue integration
- `tx_queue_wrapper.h` - Wrapper for tx-queue headers with proper Windows compatibility
- Enhanced segment buffering with transactional semantics
- Real-time statistics and performance monitoring
- Adaptive buffer sizing based on stream characteristics

### Performance Benefits

| Feature | Before | After |
|---------|--------|--------|
| IPC Method | `std::queue` with mutexes | Lock-free tx-queue transactions |
| Memory Efficiency | Standard allocations | Cache-line aligned zero-copy operations |
| Thread Safety | Mutex-based synchronization | Lock-free atomic operations |
| **Multi-Stream Performance** | **Degrades with concurrent streams** | **Scales linearly with isolated tx-queues** |
| **Data Integrity** | **Basic error handling** | **Built-in checksum validation** |
| **Throughput** | **Limited by lock contention** | **High-performance lock-free design** |

The integration works transparently with multiple streaming modes:
1. **Primary**: Demux-MPEGTS single player mode (default) - Best discontinuity recovery with optimal synchronization
2. **High-Performance**: TX-Queue IPC mode for optimal performance
3. **Professional**: TSDuck transport stream mode for professional compatibility  
4. **Legacy**: Traditional HLS segment streaming for maximum compatibility

### Demux-MPEGTS Mode (Default)

**Default Streaming Mode**: Demux-MPEGTS Mode is now the standard streaming method for better discontinuity handling:

- **Single Player Mode**: Uses one media player with external audio file support for better synchronization and performance
- **Discontinuity Recovery**: Automatically detects and recovers from stream discontinuities that cause black screen/frozen video
- **Stream Health Monitoring**: Monitors player and stream health with automatic restart capability
- **MPEG-TS Demuxing**: Uses the proven demux-mpegts library to separate elementary streams from transport streams
- **Error Recovery**: Comprehensive error handling with automatic recovery mechanisms
- **Real-time Statistics**: Detailed stream statistics and performance monitoring

This mode specifically addresses the issue where media players get stuck on a black screen or last video frame with audio still playing after discontinuities, by demuxing streams and using a single player with external audio file support for better recovery and synchronization.

### TX-Queue IPC Mode

**High-Performance Streaming Mode**: TX-Queue IPC Mode provides optimal performance for single-stream scenarios:

- **High-Performance IPC**: Lock-free producer/consumer queues between download and playback threads
- **Transactional Semantics**: Guaranteed data integrity with automatic rollback on errors
- **Named Pipe Output**: Maintains compatibility with all media players expecting stdin input
- **Adaptive Buffering**: Dynamic buffer sizing based on stream characteristics and system load
- **Zero-Copy Design**: Minimizes memory allocations and data movement

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

The application defaults to using Demux-MPEGTS mode with MPV as the media player for best discontinuity recovery.

For detailed configuration options and troubleshooting, see [DEMUX_CONFIGURATION.md](DEMUX_CONFIGURATION.md).

Default settings:
- Mode: Demux-MPEGTS single player with external audio file
- Player: `mpv.exe`
- Command: `mpv.exe --audio-file=audio.aac video.h264`
- Alternative: Legacy separate players mode also available

To change the media player or configure advanced options, see the configuration guide.

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
