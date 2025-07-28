# Tardsplaya

A Twitch stream player for Windows with enhanced TLS support.

## Features

- Multi-tab stream viewing
- Quality selection
- Windows 7+ compatibility
- **Enhanced TLS Client Support** - Custom TLS implementation for better compatibility with older Windows versions

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

The integration works transparently:
1. **Primary**: TX-Queue IPC mode for optimal performance (default)
2. **Fallback**: TSDuck transport stream mode for professional compatibility  
3. **Legacy**: Traditional HLS segment streaming for maximum compatibility

### TX-Queue IPC Mode (Default)

**Default Streaming Mode**: TX-Queue IPC Mode is now the standard streaming method:

- **High-Performance IPC**: Lock-free producer/consumer queues between download and playback threads
- **Transactional Semantics**: Guaranteed data integrity with automatic rollback on errors
- **Named Pipe Output**: Maintains compatibility with all media players expecting stdin input
- **Adaptive Buffering**: Dynamic buffer sizing based on stream characteristics and system load
- **Zero-Copy Design**: Minimizes memory allocations and data movement

TX-Queue IPC Mode provides superior performance and reliability compared to traditional mutex-based streaming.

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
