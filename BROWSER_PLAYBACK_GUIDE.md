# Browser Playback Feature - Testing Guide

## Overview
The browser playback feature allows Tardsplaya to serve Twitch streams directly to web browsers instead of external media players like MPV or VLC. This uses the native HTML5 MediaSource API to play MPEG-TS streams.

## How It Works

### 1. User Enables Browser Mode
- Go to Tools → Settings
- Check "Use browser playback (mpegts.js)"
- Click OK to save settings

### 2. Stream Playback Process
1. User loads a channel and selects quality (same as before)
2. User clicks "Watch" - instead of launching MPV/VLC:
   - HTTP server starts on port 8080+ (unique per tab)
   - Browser opens automatically to http://127.0.0.1:8080/player.html
   - Stream segments are downloaded and served via HTTP
   - Browser uses MediaSource API to play the stream

### 3. Technical Flow
```
Twitch HLS Stream → Tardsplaya Downloads Segments → HTTP Server → Browser Player
```

## Key Components

### HttpStreamServer Class (`http_server.h/cpp`)
- Lightweight HTTP server for serving stream data
- Handles multiple concurrent browser connections
- Serves HTML player page with MediaSource API integration
- Queues and serves MPEG-TS segments to browsers

### BufferAndServeStreamToBrowser Function (`stream_pipe.cpp`)
- Downloads HLS segments from Twitch
- Serves segments to HttpStreamServer for browser consumption
- Handles segment buffering and error recovery
- Similar to BufferAndPipeStreamToPlayer but serves HTTP instead of piping to stdin

### Browser Player (HTML/JavaScript)
- Uses native MediaSource API for MPEG-TS playback
- Automatic segment fetching and buffering
- Handles stream initialization and error recovery
- Standard video controls (play/pause/fullscreen)

## Benefits Over Media Players

1. **No External Dependencies**: No need to install MPV, VLC, etc.
2. **Cross-Platform**: Works on any OS with a modern browser
3. **Web-Native**: Familiar browser video controls and behavior
4. **Multiple Streams**: Easy to have multiple browser tabs/windows
5. **Integration**: Better integration with web-based workflows

## Testing

To test this feature:

1. Build Tardsplaya with the new code
2. Enable browser playback in settings
3. Load a channel and click Watch
4. Browser should open automatically with the stream
5. Verify stream plays correctly in browser
6. Test multiple concurrent streams in different tabs

## Compatibility

- **Browsers**: Chrome/Edge 23+, Firefox 42+, Safari 8+
- **Codecs**: H.264/AVC video, AAC audio (standard Twitch streams)
- **Format**: MPEG-TS container format
- **API**: HTML5 MediaSource Extensions

## Configuration Files

The browser mode setting is saved in `Tardsplaya.ini`:
```ini
[Settings]
UseBrowserPlayback=1
```

## Status Messages

When browser mode is active:
- Status bar shows "Browser Playback Ready" instead of "TX-Queue IPC Ready"
- Log messages are prefixed with "[BROWSER]"
- HTTP server port is logged for debugging

## Troubleshooting

If browser playback doesn't work:
1. Check that port 8080+ isn't blocked by firewall
2. Verify browser supports MediaSource API
3. Check browser developer console for JavaScript errors
4. Try disabling browser extensions that might interfere
5. Fall back to traditional media player mode if needed