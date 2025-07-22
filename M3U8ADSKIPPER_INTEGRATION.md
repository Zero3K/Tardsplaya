# m3u8adskipper Integration Documentation

## Overview

Tardsplaya now includes ad-skipping functionality inspired by the [m3u8adskipper](https://github.com/skimotv/m3u8adskipper) Node.js module. This feature automatically detects and skips advertisement segments in HLS (HTTP Live Streaming) content.

## How It Works

### Ad Detection Algorithm

The ad detection uses the same algorithm as m3u8adskipper:

1. **Discontinuity Detection**: Analyzes HLS playlists for `#EXT-X-DISCONTINUITY` markers, which indicate transitions between different content streams (typically between content and ads).

2. **Stream Separation**: When discontinuities are found, segments are classified into two groups based on which side of the discontinuity boundary they fall on.

3. **Content Identification**: The larger of the two groups is considered the actual content, while the smaller group is considered advertisements. This works because ad segments are typically shorter than the main content.

4. **Segment Filtering**: During streaming, only the content segments are processed and sent to the media player - ad segments are completely skipped.

### Technical Implementation

- **Enhanced HLS Parser**: Extended the existing `tsduck_hls_wrapper.cpp` with ad detection capabilities
- **Segment Classification**: Added fields to `MediaSegment` structure for ad detection metadata
- **Transport Stream Integration**: Modified the transport stream router to filter out ad segments during streaming
- **Logging**: Added comprehensive logging to show when ads are detected and skipped

## User Experience

### Status Messages

When ad detection is active, you'll see messages like:

```
[AD_SKIP] Ads detected! Total: 8, Content: 6, Ads: 2 (skipped)
[TS_MODE] Starting TSDuck transport stream routing for channel_name (quality)
```

### Behavior

- **Seamless Skipping**: Ad segments are skipped transparently - the media player receives only content segments
- **No Interruption**: Playback continues smoothly without pausing or buffering issues
- **Automatic Detection**: No user configuration required - ads are detected automatically
- **Fallback**: If no ads are detected, all segments are streamed normally

## Example Playlist

Here's an example of an HLS playlist with ads that would be detected:

```m3u8
#EXTM3U
#EXT-X-TARGETDURATION:10
#EXT-X-VERSION:3

#EXTINF:10.0,
content_segment_1.ts
#EXTINF:10.0,
content_segment_2.ts

#EXT-X-DISCONTINUITY
#EXTINF:5.0,
ad_segment_1.ts
#EXTINF:5.0,
ad_segment_2.ts

#EXT-X-DISCONTINUITY
#EXTINF:10.0,
content_segment_3.ts
#EXTINF:10.0,
content_segment_4.ts
```

In this example:
- **Content segments**: `content_segment_1.ts`, `content_segment_2.ts`, `content_segment_3.ts`, `content_segment_4.ts` (4 segments)
- **Ad segments**: `ad_segment_1.ts`, `ad_segment_2.ts` (2 segments) - These would be **SKIPPED**

## Integration with Existing Features

- **TSDuck Transport Stream Mode**: Ad skipping works with the default TSDuck transport stream routing
- **Multi-Stream Support**: Each stream tab handles ad detection independently
- **Frame Number Tagging**: Ad transitions are handled properly with frame tracking reset
- **Low-Latency Mode**: Ad skipping is compatible with low-latency streaming optimizations

## Limitations

- **Requires Discontinuities**: Ad detection only works with streams that use `#EXT-X-DISCONTINUITY` markers
- **Pattern-Based**: Detection relies on the assumption that ads are shorter than content segments
- **Live Streams**: Works best with VOD content; live stream ad detection may vary depending on the streaming service

## Technical Details

### Files Modified

- `tsduck_hls_wrapper.h` - Added ad detection structures and methods
- `tsduck_hls_wrapper.cpp` - Implemented m3u8adskipper algorithm with multi-stream awareness
- `tsduck_transport_router.cpp` - Integrated ad filtering into stream processing
- `tsduck_transport_router.h` - Added stream instance isolation

### Key Methods

- `PlaylistParser::DetectAds(bool conservative_mode)` - Main ad detection entry point with multi-stream support
- `PlaylistParser::GetContentSegments()` - Returns only non-ad segments
- `PlaylistParser::GetAdDetectionStats()` - Provides detection statistics including reliability info
- `PlaylistParser::ClassifySegmentsByDiscontinuity()` - Groups segments by stream
- `PlaylistParser::DetermineContentGroup()` - Identifies which group contains content
- `PlaylistParser::ValidateAdDetectionResult()` - Validates detection confidence

## Multi-Stream Improvements (v2.0)

### Issue Resolution

**Problem**: Ad detection became unreliable when multiple stream instances were running concurrently due to resource contention and timing issues.

**Solution**: Implemented multi-stream aware ad detection with conservative fallbacks and system load detection.

### New Features

#### Conservative Mode
- **Automatic Activation**: Enabled when multiple streams are active or system is under load
- **Stricter Thresholds**: Requires 3:1 content-to-ad ratio (vs 2:1 in normal mode)
- **Better Validation**: Enhanced checks for segment patterns and discontinuity counts
- **Fallback Logic**: Automatically streams all segments if detection is unreliable

#### Stream Instance Isolation
- **Unique Stream IDs**: Each stream gets a unique identifier (`stream_0`, `stream_1`, etc.)
- **Independent Logging**: Clear separation of log messages between stream instances
- **Per-Stream Statistics**: Ad detection metrics tracked separately for each stream

#### System Load Detection
- **Active Stream Monitoring**: Tracks concurrent stream count via `StreamResourceManager`
- **Resource Pressure Detection**: Identifies when system is under load
- **Adaptive Behavior**: Automatically switches modes based on system conditions

### Enhanced Status Messages

#### Normal Operation (Single Stream)
```
[stream_0] System status: 1 active streams, load=NORMAL
[stream_0:AD_SKIP] Normal mode: Ads detected! Total: 8, Content: 6, Ads: 2 (skipped)
```

#### Conservative Mode (Multiple Streams)
```
[stream_1] System status: 3 active streams, load=HIGH
[stream_1:AD_SKIP] Conservative mode: Ads detected! Total: 12, Content: 9, Ads: 3 (skipped)
[stream_2:AD_SKIP] Conservative mode: 4 discontinuities found but no reliable ad pattern detected
```

#### Reliability Fallback
```
[stream_0:AD_SKIP] Ad detection unreliable - using all segments for stability
[stream_0:AD_SKIP] Reason: Conservative mode: insufficient confidence (ratio 1.5 < 3.0)
```

### Benefits

1. **Reliable Multi-Stream Operation**: No false ad detection when multiple streams are running
2. **Automatic Adaptation**: System adjusts behavior based on load without user intervention
3. **Stream Isolation**: Each stream operates independently without interference
4. **Graceful Degradation**: Falls back to normal playback if ad detection becomes uncertain
5. **Performance Optimization**: Reduces overhead when system resources are constrained

### Testing Results

✅ **Single Stream**: Normal ad detection with 2:1 confidence threshold  
✅ **Multiple Streams**: Conservative detection with 3:1 confidence threshold  
✅ **Resource Pressure**: Automatic fallback to normal playback when detection is unreliable  
✅ **Stream Isolation**: Independent operation without cross-stream interference  
✅ **Edge Cases**: Proper handling of ambiguous discontinuity patterns  

This implementation provides the same core functionality as the original m3u8adskipper module while being fully integrated into Tardsplaya's existing streaming infrastructure and optimized for reliable multi-stream operation.