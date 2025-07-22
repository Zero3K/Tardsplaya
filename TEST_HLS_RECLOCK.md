# Testing HLS PTS Reclock Tool

## Quick Test Instructions

### 1. Build the Tool
- Open Tardsplaya.sln in Visual Studio
- Build the HLSPTSReclock project (should create `hls-pts-reclock.exe`)

### 2. Test with Verbose Output
Try running the tool directly with a sample HLS URL to see detailed output:

```cmd
hls-pts-reclock.exe --verbose -i "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8" --stdout
```

### 3. Expected Behavior

**If working correctly, you should see:**
```
HLS PTS Discontinuity Reclock Tool
==================================

Processing HLS stream: https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8
Output: - (format: mpegts)
Downloading playlist from: https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8
Downloaded XXX bytes of playlist data
Found X segments in playlist
...
Streaming mpegts to stdout...
```

**If HTTPS issues persist, you'll see:**
```
Failed to download HLS playlist from: https://...
This could be due to:
  - Network connectivity issues
  - Invalid URL or server not responding
  - TLS/SSL certificate problems
  - Server blocking the request
```

### 4. Test Integration with Tardsplaya
- Enable Debug mode in Tardsplaya (sets `g_verboseDebug = true`)
- Try streaming a Twitch channel that uses HLS
- Check the log output for HLS PTS correction messages

### 5. Debugging Information
The tool now provides detailed logging:
- Download progress and sizes
- Playlist content preview
- TS packet parsing results
- First few bytes of generated output
- Detailed error messages with possible causes

### 6. Fallback Behavior
If TS parsing fails, the tool now falls back to passing through original segment data, so streams should work even if correction fails.

## Common Issues and Solutions

**"Failed to render the file" in MPC-HC:**
- Check if the verbose output shows valid TS packets (should start with 0x47)
- Verify segments are being downloaded successfully
- Look for TS packet reconstruction warnings

**HTTPS connection failures:**
- Test with HTTP URLs first to isolate TLS issues
- Check Windows certificate store for expired/invalid certificates
- Try different test streams to verify it's not server-specific

**No output generated:**
- Check if playlist parsing found any segments
- Verify segment URLs are absolute/resolved correctly
- Look for TS packet parsing success messages