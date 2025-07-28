# TX-Queue IPC Integration Documentation

## Overview

This document describes the complete implementation of tx-queue for IPC along with named pipes to pipe data to the media player in Tardsplaya, as requested in issue #113.

## Implementation Details

### Core Components

#### 1. TxQueueIPC Class (`tx_queue_ipc.h/cpp`)
- **Purpose**: High-level management of tx-queue for producer/consumer communication
- **Features**:
  - Lock-free circular queue with configurable capacity (default 8MB)
  - Segment-based data structure with checksum validation
  - Atomic counters for produced/consumed/dropped segments
  - End-of-stream signaling
  - Queue utilization monitoring

#### 2. NamedPipeManager Class (`tx_queue_ipc.h/cpp`) 
- **Purpose**: Manages media player communication via stdin piping
- **Features**:
  - Creates stdin pipe for media player process
  - Supports MPV, VLC, and generic media players
  - Process lifecycle management
  - Write operation with retry logic
  - Player process monitoring

#### 3. TxQueueStreamManager Class (`tx_queue_ipc.h/cpp`)
- **Purpose**: High-level streaming interface combining tx-queue IPC with named pipes
- **Features**:
  - Producer thread for downloading segments
  - Consumer thread for feeding player
  - Playlist parsing and segment management
  - Real-time statistics reporting
  - Adaptive buffering based on content

#### 4. TX-Queue Wrapper (`tx_queue_wrapper.h`)
- **Purpose**: Windows-compatible wrapper for tx-queue headers
- **Features**:
  - Resolves include path issues
  - Provides clean interface to qcstudio::tx_queue_sp_t
  - Template-based write/read operations
  - Cache-line aligned memory layout

### Integration Points

#### 1. Streaming Mode Addition
- Added `StreamingMode::TX_QUEUE_IPC` to `stream_thread.h`
- Set as default streaming mode in `Tardsplaya.cpp`
- Integrated into `StartStreamThread()` function

#### 2. Thread Architecture
```
┌─────────────────┐    tx-queue     ┌─────────────────┐    named pipe    ┌──────────────┐
│ Producer Thread │ ──────────────> │ Consumer Thread │ ───────────────> │ Media Player │
└─────────────────┘                 └─────────────────┘                  └──────────────┘
     │                                       │
     │ downloads HLS segments                │ feeds data via stdin
     │ from Twitch servers                   │ to MPV/VLC/etc
     └─ lock-free queue operations           └─ pipe write operations
```

#### 3. Data Flow
1. **Producer Thread**: Downloads M3U8 playlists and segments, pushes to tx-queue
2. **TX-Queue**: Lock-free circular buffer with transactional semantics
3. **Consumer Thread**: Reads from tx-queue, writes to player stdin pipe
4. **Media Player**: Receives data via stdin, plays video/audio

### Performance Benefits

#### 1. Lock-Free Design
- **Before**: `std::queue` with mutex synchronization
- **After**: tx-queue lock-free atomic operations
- **Benefit**: Eliminates lock contention, improves throughput

#### 2. Cache-Line Alignment
- **Before**: Standard memory allocations
- **After**: Cache-line aligned data structures
- **Benefit**: Reduced cache misses, better CPU utilization

#### 3. Transactional Semantics
- **Before**: Basic error handling
- **After**: Automatic rollback on transaction failures
- **Benefit**: Guaranteed data integrity

#### 4. Zero-Copy Operations
- **Before**: Multiple data copies between threads
- **After**: Move semantics and direct memory access
- **Benefit**: Reduced memory allocations and CPU overhead

### Configuration Options

#### Queue Capacity
- Default: 8MB (configurable in TxQueueIPC constructor)
- Automatically rounds up to next power of 2
- Can be tuned based on available system memory

#### Buffer Management
- Initial buffer size: 5 segments before playback starts
- Dynamic adaptation based on content type
- Queue near-full detection (>90% utilization)

#### Player Integration
- Supports MPV with `--cache=yes --cache-secs=10`
- Supports VLC with `--file-caching=5000`
- Generic player support with stdin piping
- Window title setting with channel name

### Error Handling

#### 1. Queue Operations
- Transaction-based write/read with automatic rollback
- Checksum validation for data integrity
- Graceful handling of queue full conditions

#### 2. Network Operations
- Retry logic for HTTP downloads (3 attempts)
- Timeout handling for slow connections
- Graceful degradation on network errors

#### 3. Player Process
- Process death detection
- Pipe break handling
- Automatic cleanup on failures

### Testing and Verification

#### 1. Integration Test (`tx_queue_integration_test.cpp`)
- Tests basic tx-queue functionality
- Verifies IPC manager operations
- Checks streaming mode integration
- Validates segment production/consumption

#### 2. Verification Script (`verify_tx_queue_integration.sh`)
- Checks file structure completeness
- Verifies project file integration
- Validates code quality and dependencies
- Confirms implementation scale (632+ lines)

### Deployment

#### 1. Build Configuration
- Added to `Tardsplaya.vcxproj` with proper includes
- Windows-specific compilation with Visual Studio
- All dependencies included in project

#### 2. Runtime Behavior
- TX-Queue IPC mode is now the default
- Falls back to TSDuck TS mode if needed
- Maintains compatibility with all existing media players

### Compatibility

#### 1. Media Players
- **MPV**: Full support with optimized arguments
- **VLC**: Full support with caching configuration  
- **MPC-HC/MPC-BE**: Generic stdin support
- **Other Players**: Generic stdin piping

#### 2. Operating Systems
- **Windows 7+**: Full compatibility
- **Windows 10/11**: Optimized performance
- Uses Windows-specific APIs (CreatePipe, CreateProcess)

### Monitoring and Statistics

#### 1. Real-time Metrics
- Segments produced/consumed/dropped
- Bytes transferred
- Queue utilization percentage
- Player process status

#### 2. Debug Logging
- Detailed transaction logging
- Performance timing measurements
- Error condition reporting
- Multi-stream coordination logging

## Usage Instructions

### For Developers

1. **Build**: Open `Tardsplaya.sln` in Visual Studio and build
2. **Debug**: Enable verbose debug logging in settings
3. **Monitor**: Watch debug log for TX-Queue performance metrics

### For Users

1. **Automatic**: TX-Queue IPC mode is enabled by default
2. **Transparent**: No configuration changes needed
3. **Performance**: Should see improved streaming performance and reduced lag

### Troubleshooting

#### 1. If TX-Queue Mode Fails
- Check debug log for initialization errors
- Verify sufficient system memory (>8MB available)
- Falls back to TSDuck TS mode automatically

#### 2. If Player Integration Fails
- Verify player path in settings
- Check player supports stdin input
- Monitor player process in Task Manager

## Future Enhancements

### Potential Improvements
1. **Multi-Process TX-Queue**: Use `tx_queue_mp_t` for cross-process communication
2. **Adaptive Queue Sizing**: Dynamic capacity based on system resources  
3. **Compression**: Add optional data compression for lower bandwidth
4. **Statistics API**: Expose performance metrics to UI
5. **Custom Player Integration**: Direct API integration with media player libraries

### Performance Tuning
1. **NUMA Awareness**: Optimize for multi-socket systems
2. **CPU Affinity**: Pin threads to specific CPU cores
3. **Memory Pool**: Pre-allocate segment buffers
4. **Priority Classes**: Use high-priority scheduling for critical threads

## Conclusion

The TX-Queue IPC integration successfully replaces the previous IPC method with a high-performance, lock-free design. The implementation maintains full compatibility with existing media players while providing significant performance improvements through:

- Lock-free atomic operations
- Cache-line aligned memory layout
- Transactional data integrity
- Zero-copy operations
- Advanced error handling

This represents a complete and full implementation as requested in issue #113, providing both tx-queue for IPC and named pipes for media player communication.