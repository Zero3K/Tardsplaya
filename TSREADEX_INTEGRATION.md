# TSReadEX Integration Documentation

## Overview

Tardsplaya now includes optional integration with TSReadEX, a sophisticated MPEG-TS processing tool that provides stream filtering, enhancement, and stabilization capabilities.

## What is TSReadEX?

TSReadEX (https://github.com/xtne6f/tsreadex) is a tool for MPEG-TS stream selection and stabilization. It can:

- Filter and select specific services/programs from transport streams
- Standardize PID assignments for better player compatibility  
- Ensure consistent audio streams (complement missing audio)
- Remove unwanted data like program guide information
- Handle audio stream manipulation (mono-to-stereo conversion)
- Stabilize stream structure for improved playback reliability

## Integration Benefits

For Tardsplaya users, TSReadEX provides:

1. **Improved Stream Compatibility**: Standardized PID assignments work better with various media players
2. **Cleaner Streams**: Removes unnecessary program guide data to reduce bandwidth
3. **Better Audio Handling**: Ensures consistent audio streams for reliable playback
4. **Enhanced Stability**: Stream processing that handles edge cases in transport stream format

## How to Enable

1. Go to **Tools → Settings**
2. In the "TSReadEX Stream Enhancement" section:
   - Check **"Enable TSReadEX processing"**
   - Optionally configure:
     - **"Remove program guide data"** - Removes EIT data for cleaner streams (recommended)
     - **"Stabilize audio streams"** - Ensures consistent audio presence (recommended)
3. Click **OK** to save settings

## When TSReadEX is Active

- Streaming mode automatically switches to **Transport Stream** mode 
- TSReadEX processes the stream before sending to media player
- Log messages will show **[TS_ROUTER]** and **[TSREADEX]** entries
- Stream filtering statistics are logged periodically

## Performance Impact

- Minimal CPU overhead for stream processing
- Slightly increased memory usage for buffering
- May reduce overall bandwidth due to filtered data
- Transport Stream mode has different buffering characteristics than TX-Queue mode

## Troubleshooting

If you experience issues with TSReadEX enabled:

1. **Disable TSReadEX** in settings to return to standard TX-Queue mode
2. Check logs for **[TSREADEX]** error messages
3. Verify your media player supports transport stream input
4. Some very unusual stream formats may not be compatible

## Technical Details

- TSReadEX processes streams in the Transport Stream pipeline
- Works as optional post-processing filter 
- Maintains compatibility with existing streaming infrastructure
- Automatically falls back to original packets if processing fails

The integration provides these capabilities without disrupting Tardsplaya's core functionality, allowing users to enable enhanced stream processing when needed.