# Datapath IPC Implementation Guide

## Overview

This document describes the new Datapath-based IPC implementation that replaces traditional Windows pipes for high-performance streaming to media players. The implementation uses the [Datapath library](https://github.com/Xaymar/datapath) for optimized inter-process communication.

## Features

- **High Performance**: Datapath provides low-latency, high-throughput IPC (~100µs latency)
- **Named Pipe Compatibility**: Media players can connect via standard named pipes
- **Multiple Streams**: Supports concurrent streaming to multiple media players
- **Robust Error Handling**: Advanced error detection and recovery mechanisms
- **Configurable Buffering**: Dynamic buffer management based on stream characteristics

## Architecture

### Components

1. **DatapathIPC Class**: Main IPC implementation using Datapath library
2. **Named Pipe Bridge**: Translates Datapath data to named pipes for media player compatibility
3. **Legacy Fallback**: Original Windows pipe implementation preserved for compatibility

### Data Flow

```
HLS Stream → Tardsplaya → Datapath Server → Named Pipe → Media Player
```

## Configuration

### Enabling Datapath IPC

In `stream_pipe.cpp`, the implementation is controlled by:

```cpp
static const bool USE_DATAPATH_IPC = true;  // Use Datapath (default)
                                            // false = Use legacy pipes
```

### Configuration Options

The `DatapathIPC::Config` structure provides various settings:

```cpp
struct Config {
    std::wstring channel_name;           // Channel name for identification
    std::wstring player_path;            // Media player executable path
    std::wstring datapath_name;          // Unique Datapath server name
    std::wstring named_pipe_path;        // Named pipe path
    size_t max_buffer_segments = 10;    // Maximum buffered segments
    size_t segment_timeout_ms = 5000;   // Segment operation timeout
    size_t connection_timeout_ms = 10000; // Connection timeout
    bool use_named_pipe_bridge = true;  // Enable named pipe bridge
};
```

## Usage Scenarios

### Scenario 1: Standard Media Players (MPV, VLC, MPC-HC)

For media players that read from named pipes:

```
Tardsplaya creates: \\.\pipe\TardsplayaStream_<channel_name>
Media Player command: mpv.exe \\.\pipe\TardsplayaStream_<channel_name>
```

The named pipe is created automatically when streaming starts and remains available until streaming stops.

### Scenario 2: Custom Integration

For applications with direct Datapath support:

```cpp
// Connect to Datapath server
std::shared_ptr<datapath::isocket> socket;
datapath::connect(socket, "TardsplayaDatapath_" + channel_name);

// Set up data handler
socket->on_message.add([](const std::vector<char>& data) {
    // Process stream data
});
```

## Build Configuration

### Visual Studio Projects

The solution includes the main application:

1. **Tardsplaya.vcxproj**: Main application with integrated Datapath IPC and named pipe support

### Dependencies

- **advapi32.lib**: Required by Datapath for Windows APIs
- **Datapath Source Files**: Included directly in the project

### Include Directories

```
datapath\include
datapath\source
```

## API Reference

### DatapathIPC Class

#### Methods

- `bool Initialize(const Config& config)`: Initialize the IPC system
- `bool StartStreaming(...)`: Start streaming to media player
- `void StopStreaming()`: Stop streaming and cleanup
- `bool WriteSegmentData(...)`: Write segment data to stream
- `size_t GetBufferSize()`: Get current buffer size
- `std::wstring GetStatusInfo()`: Get status information


## Error Handling

### Connection Errors

- **datapath::error::InvalidPath**: Server name not found
- **datapath::error::TimedOut**: Connection timeout
- **datapath::error::Closed**: Connection closed by server

### Recovery Mechanisms

- Automatic retry for failed connections
- Buffer overflow protection
- Client disconnection handling
- Graceful shutdown on errors

## Performance Tuning

### Buffer Management

```cpp
config.max_buffer_segments = 15;  // Increase for high-bitrate streams
config.segment_timeout_ms = 3000;  // Reduce for low-latency
```

### Thread Configuration

The implementation uses multiple threads:

- **Server Thread**: Manages Datapath server and client connections
- **Buffer Manager**: Handles segment buffering and distribution
- **Named Pipe Thread**: Manages named pipe connections
- **Media Player Monitor**: Monitors media player process

## Debugging

### Debug Logging

Enable debug logging to see IPC operations:

```cpp
AddDebugLog(L"[DATAPATH] " + message);
```

### Status Information

Get detailed status with:

```cpp
std::wstring status = datapath_ipc.GetStatusInfo();
// Example: "DatapathIPC[channel]: Active=true, Buffer=5/10, Clients=1"
```

### Common Issues

1. **Connection Refused**: Check if server name is correct
2. **Buffer Overflow**: Increase max_buffer_segments
3. **Player Not Starting**: Verify player path and named pipe support
4. **High Latency**: Reduce buffer size or timeout values

## Migration from Legacy Pipes

### Compatibility

The new implementation maintains full compatibility with existing code:

- Same function signatures in `stream_pipe.h`
- Automatic fallback to legacy implementation if needed
- No changes required in calling code

### Testing

To test both implementations:

```cpp
// Test Datapath IPC
static const bool USE_DATAPATH_IPC = true;

// Test legacy pipes
static const bool USE_DATAPATH_IPC = false;
```

## License Considerations

The Datapath library is licensed under AGPL v3. This means:

- Source code must be available if distributing binaries
- Network use may trigger copyleft requirements
- Consider licensing implications for commercial use

## Future Enhancements

Planned improvements:

1. **Cross-Platform Support**: Linux and macOS versions
2. **Compression**: Optional data compression for bandwidth efficiency
3. **Encryption**: Optional data encryption for security
4. **Metrics**: Detailed performance metrics and monitoring
5. **Dynamic Configuration**: Runtime configuration changes

## Support

For issues or questions:

1. Check debug logs for error messages
2. Verify media player compatibility with named pipes
3. Test with legacy pipe implementation for comparison
4. Report issues with detailed logs and system information