# Ad-Based Quality Switching

This feature automatically switches to a lower quality stream during advertisements and returns to your selected quality when ads finish.

## How It Works

1. **Ad Detection**: The application detects ads using SCTE-35 markers in the HLS stream
   - `#EXT-X-SCTE35-OUT` signals the start of an ad break
   - `#EXT-X-SCTE35-IN` signals the end of an ad break

2. **Quality Selection**: 
   - When you select a quality and start watching, your choice is saved as the "user quality"
   - The system automatically selects the lowest available quality (preferring audio-only) as the "ad quality"

3. **Automatic Switching**:
   - When ads start: Stream switches to the ad quality (typically audio-only or lowest resolution)
   - When ads end: Stream switches back to your originally selected quality

## Status Indicators

- **Status Bar**: Shows "[AD MODE] channel - Lower quality active" when ads are playing
- **Debug Log**: Contains detailed information about ad detection and quality switches (enable verbose logging in settings)

## Benefits

- **Reduced Bandwidth**: Lower quality during ads saves bandwidth
- **Faster Loading**: Ads load faster with lower quality
- **Seamless Experience**: Automatic switching without manual intervention
- **Original Quality Restored**: Your preferred quality is automatically restored after ads

## Technical Details

- Uses TSDuck-inspired HLS parsing for reliable ad detection
- Works with both traditional HLS streaming and Transport Stream mode
- Compatible with all supported media players (MPV, VLC, MPC-HC, MPC-BE)
- Thread-safe implementation for multi-stream support

## Troubleshooting

If ad switching isn't working:
1. Enable verbose debugging in Tools → Settings
2. Check the debug log for ad detection messages
3. Ensure the stream contains SCTE-35 markers (some streams may not have them)
4. Verify multiple qualities are available for the channel

## Notes

- Not all Twitch streams contain SCTE-35 ad markers
- The feature works best with streams that have multiple quality options
- Audio-only streams may not trigger quality switching since they're already minimal quality