# Memory-Backed File Streaming Implementation

This implementation replaces the HTTP piping system with memory-backed files for improved reliability and performance.

## Architecture

### Before (HTTP Piping)
```
Tardsplaya.exe → HTTP Server (localhost:8080+) → Media Player
```

### After (Memory-Backed Files)
```
Tardsplaya.exe → Memory Map → TardsplayaViewer.exe → Media Player (stdin)
```

## Components

### 1. Tardsplaya.exe (Main Application)
- Creates memory-mapped files as writer
- Downloads and buffers stream segments
- Writes stream data to memory map
- Launches TardsplayaViewer.exe for each stream

### 2. TardsplayaViewer.exe (Viewer Program)
- Reads stream data from memory-mapped files
- Launches media player with stdin pipe
- Transfers data from memory map to media player
- Handles cleanup when stream ends

### 3. Memory Map System
- **StreamMemoryMap**: Core memory-mapped file implementation
- **StreamMemoryMapUtils**: Helper functions for launching viewers
- **stream_memory_pipe**: Memory-based streaming functions

## Files Modified/Added

### New Files
- `TardsplayaViewer.cpp` - Standalone viewer program
- `TardsplayaViewer.vcxproj` - Viewer project file
- `stream_memory_pipe.h/.cpp` - Memory-based streaming implementation
- `TardsplayaComplete.sln` - Solution file for both projects

### Modified Files
- `stream_pipe.h/.cpp` - Updated to use memory maps instead of HTTP
- `stream_memory_map.cpp` - Implemented helper creation function
- `Tardsplaya.vcxproj` - Added new source files

## Benefits

### 1. Reliability
- No more port conflicts between multiple streams
- No dependency on HTTP stack
- Better error handling and recovery

### 2. Performance
- Direct memory access (faster than HTTP)
- No network layer overhead
- Better multi-stream support

### 3. Security
- No localhost HTTP servers
- No open network ports
- Better isolation between streams

### 4. Compatibility
- Works with all media players that support stdin
- No changes needed to media player configuration
- Better Windows 7 compatibility

## Memory Map Details

### Structure
```cpp
struct ControlHeader {
    volatile LONG writer_position;      // Current write position
    volatile LONG reader_position;      // Current read position  
    volatile LONG buffer_size;          // Total buffer size
    volatile LONG data_available;       // Amount of data available
    volatile LONG stream_ended;         // Stream end flag
    volatile LONG writer_active;        // Writer process active flag
    volatile LONG reader_active;        // Reader process active flag
    volatile LONG sequence_number;      // For debugging/validation
};
```

### Naming Convention
- Memory maps are named: `TardsplayaStream_{channel_name}`
- Mutexes are named: `TardsplayaStream_{channel_name}_Mutex`

### Buffer Management
- Default buffer size: 16MB
- Header size: 4KB (for control data)
- Circular buffer with wrap-around
- Thread-safe with mutex synchronization

## Usage

### Building
```bash
# Build both projects
msbuild TardsplayaComplete.sln /p:Configuration=Release

# Or build individually
msbuild Tardsplaya.vcxproj /p:Configuration=Release
msbuild TardsplayaViewer.vcxproj /p:Configuration=Release
```

### Deployment
Both `Tardsplaya.exe` and `TardsplayaViewer.exe` must be in the same directory.

### Testing
1. Launch Tardsplaya.exe
2. Load a stream (memory map will be created automatically)
3. TardsplayaViewer.exe will be launched automatically
4. Media player receives stream via stdin

## Debugging

### Enable Verbose Logging
- Go to Tools → Settings in Tardsplaya
- Check "Verbose Debug" option
- Logs will show memory map operations

### Log Prefixes
- `[MEMORY_STREAMS]` - Stream management
- `[MEMORY_DOWNLOAD]` - Segment downloading
- `[MEMORY_FEEDER]` - Memory map writing
- `[VIEWER]` - TardsplayaViewer operations

### Common Issues

1. **TardsplayaViewer.exe not found**
   - Ensure both executables are in same directory
   - Check file permissions

2. **Memory map creation failed**
   - Check available system memory
   - Verify Windows permissions for memory mapping

3. **Media player not launching**
   - Verify media player path in settings
   - Check that player supports stdin input

## Backwards Compatibility

The HTTP-based streaming function is still available as `BufferAndPipeStreamToPlayerHTTP()` if needed for debugging or fallback scenarios.

## Future Improvements

1. **Dynamic Buffer Sizing**: Adjust buffer size based on stream bitrate
2. **Compression**: Optional compression for memory map data
3. **Multi-Reader Support**: Allow multiple viewers per memory map
4. **Shared Memory Pools**: Reuse memory maps across streams