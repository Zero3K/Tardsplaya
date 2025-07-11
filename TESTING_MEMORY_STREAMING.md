# Memory-Backed File Streaming Test Guide

## Testing the New Implementation

### Prerequisites
- Windows environment with Visual Studio 2019+
- Media player (MPV recommended: https://mpv.io/installation/)
- Twitch channel to test with (e.g., any popular live channel)

### Build Instructions

1. **Open the solution:**
   ```
   Open TardsplayaComplete.sln in Visual Studio
   ```

2. **Build both projects:**
   ```
   Build → Build Solution (Ctrl+Shift+B)
   ```
   
   This should create:
   - `Tardsplaya.exe` (main application)
   - `TardsplayaViewer.exe` (memory map viewer)

3. **Verify both files are in same directory:**
   ```
   bin/Release/Tardsplaya.exe
   bin/Release/TardsplayaViewer.exe
   ```

### Testing Steps

#### Test 1: Basic Memory Map Streaming
1. Launch `Tardsplaya.exe`
2. Enter a live Twitch channel name (e.g., "shroud")
3. Click "Load" to fetch stream qualities
4. Select a quality and click "Watch"
5. **Expected behavior:**
   - Memory map should be created (`TardsplayaStream_{channel}`)
   - `TardsplayaViewer.exe` should launch automatically
   - Media player should start and play the stream
   - No HTTP localhost URLs in debug logs

#### Test 2: Multi-Stream Support
1. Use File → New Stream (Ctrl+T) to open second tab
2. Load different channel in new tab
3. Start both streams simultaneously
4. **Expected behavior:**
   - Two separate memory maps created
   - Two `TardsplayaViewer.exe` processes running
   - Two media players playing different streams
   - No port conflicts or interference

#### Test 3: Stream Termination
1. Click "Stop" on an active stream
2. **Expected behavior:**
   - Memory map writer signals end
   - `TardsplayaViewer.exe` detects end and exits
   - Media player closes gracefully
   - Memory map is cleaned up

#### Test 4: Error Handling
1. Start a stream without MPV installed
2. **Expected behavior:**
   - `TardsplayaViewer.exe` should log error about player launch failure
   - Main app should detect viewer failure and clean up
   - No hanging processes or memory maps

### Debug Information

#### Enable Verbose Logging
1. Go to Tools → Settings
2. Check "Verbose Debug" option
3. Click OK

#### Key Log Entries to Look For

**Memory Map Creation:**
```
[DEBUG] StreamMemoryMap::CreateAsWriter: Creating memory map TardsplayaStream_{channel}
[DEBUG] BufferAndStreamToPlayerViaMemoryMap: Created memory map for {channel}
```

**Viewer Launch:**
```
[DEBUG] LaunchPlayerWithMemoryMap: Command: "TardsplayaViewer.exe" "TardsplayaStream_{channel}" "mpv.exe"
[DEBUG] LaunchPlayerWithMemoryMap: Successfully launched for {channel}
```

**Data Streaming:**
```
[DEBUG] [MEMORY_FEEDER] Fed segment to memory map, local_buffer=X for {channel}
[VIEWER] Starting data streaming to media player...
[VIEWER] Media player launched successfully, PID=XXXX
```

**Stream End:**
```
[DEBUG] [MEMORY_FEEDER] *** FEEDER THREAD ENDING *** for {channel}
[VIEWER] Viewer ending, total bytes streamed: XXXXX
```

### Comparison with HTTP Method

To verify the improvement, you can temporarily switch back to HTTP method:

1. In `stream_pipe.cpp`, change `BufferAndPipeStreamToPlayer` to call:
   ```cpp
   return BufferAndPipeStreamToPlayerHTTP(
       player_path, playlist_url, cancel_token, buffer_segments, channel_name, chunk_count
   );
   ```

2. Compare behavior:
   - HTTP: Uses `localhost:8080+` URLs
   - Memory: No network traffic, direct memory access

### Performance Validation

#### Memory Usage
- Check Task Manager for memory usage
- Each stream should use ~16MB for memory map + buffer
- No growth over time (no memory leaks)

#### Process Count
- HTTP method: 1 main process per stream
- Memory method: 1 main process + 1 viewer process per stream

#### Network Traffic
- HTTP method: Localhost traffic visible in Resource Monitor
- Memory method: No localhost traffic

### Troubleshooting

#### "TardsplayaViewer.exe not found"
- Ensure both executables are in same directory
- Check file permissions and antivirus exclusions

#### "Failed to create memory map"
- Verify available system memory (16MB per stream)
- Check Windows permissions for memory mapping
- Ensure unique stream names (no conflicts)

#### "Failed to launch media player"
- Verify MPV is installed and in PATH
- Try absolute path to player executable
- Check that player supports stdin input (`mpv.exe -`)

#### Player launches but no video
- Enable MPV verbose logging: `mpv.exe --msg-level=all=v -`
- Check that stream data is being written to pipe
- Verify memory map has data available

### Expected Test Results

✅ **Success Indicators:**
- Streams play without localhost URLs
- Multiple streams work simultaneously  
- Clean shutdown when stopping streams
- No port conflicts between streams
- Faster startup compared to HTTP method

❌ **Failure Indicators:**
- Hanging `TardsplayaViewer.exe` processes
- Memory maps not cleaned up after stream end
- Media player launches but shows "loading" indefinitely
- Error messages about pipe communication

### Manual Verification

You can manually test the memory map system:

1. **Start main app and begin a stream**
2. **Check memory maps exist:**
   ```cmd
   # Use Process Explorer or similar tool to view memory maps
   # Look for "TardsplayaStream_{channel}" entries
   ```

3. **Monitor viewer process:**
   ```cmd
   tasklist | findstr TardsplayaViewer
   ```

4. **Test viewer directly:**
   ```cmd
   TardsplayaViewer.exe "TardsplayaStream_testchannel" "mpv.exe"
   ```

This implementation provides a robust replacement for HTTP piping that should be more reliable, faster, and better suited for multi-stream scenarios.