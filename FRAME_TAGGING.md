# Frame Number Tagging Documentation

## Overview

Frame Number Tagging is a comprehensive performance monitoring and lag diagnosis system implemented in Tardsplaya to help identify and troubleshoot streaming issues. This feature provides detailed tracking of individual video segments, timing analysis, and performance metrics to help diagnose the root causes of lag and buffering problems.

## Features

### 1. Sequential Frame Numbering
- Every video segment (frame) is assigned a unique sequential number
- Enables precise tracking of which segments cause issues
- Helps identify missing or dropped frames

### 2. Performance Metrics
- **Download Times**: Track how long each segment takes to download
- **Success Rates**: Monitor what percentage of downloads succeed
- **Average/Maximum Times**: Calculate performance statistics
- **Rolling Window**: Maintain recent performance history (last 50 segments)

### 3. Lag Detection
- **Automatic Warnings**: System alerts when performance degrades
- **Threshold Monitoring**: Detects when download times exceed acceptable limits
- **Reliability Tracking**: Monitors download success rates

### 4. Ad Segment Tracking
- **Ad Skip Monitoring**: Track when ad segments are replaced with placeholders
- **Timing Impact**: Measure how ad replacement affects stream timing
- **Sequence Continuity**: Monitor for gaps in stream continuity

## Log Output Examples

### Normal Operation
```
[FRAME_TAG] Frame tracking initialized for channelname
[FRAME_TAG] Frame #1 [CONTENT] - Total processed: 1, Success rate: 100%
[FRAME_TAG] Frame download time: 120ms, Avg: 120ms, Max: 120ms, Success rate: 100%
```

### Ad Segment Handling
```
[FRAME_TAG] Frame #15 [AD_PLACEHOLDER] - Total processed: 15, Ad segments skipped: 3
[FRAME_TAG] Ad placeholder generated in 2ms
```

### Performance Summary (Every 30 Frames)
```
[FRAME_TAG] *** PERFORMANCE SUMMARY ***
[FRAME_TAG] Stream: channelname, Duration: 62s
[FRAME_TAG] Total frames: 30, Processed: 30, Downloaded: 28
[FRAME_TAG] Ad segments skipped: 2, Sequence gaps: 0
[FRAME_TAG] Download times - Avg: 145ms, Max: 890ms
[FRAME_TAG] Success rate: 93.33%
```

### Lag Warnings
```
[FRAME_TAG] *** LAG WARNING *** Average download time > 2s, may cause buffering issues
[FRAME_TAG] *** RELIABILITY WARNING *** Download success rate < 90%, may cause playback issues
[FRAME_TAG] *** CONTINUITY WARNING *** Multiple sequence gaps detected, stream may be unstable
```

## Using Frame Number Tagging for Troubleshooting

### 1. Identifying Slow Downloads
Look for frames with high download times:
```
[FRAME_TAG] Frame download time: 3420ms, Avg: 456ms, Max: 3420ms
```
This indicates a specific segment took 3.4 seconds to download, which could cause buffering.

### 2. Detecting Network Issues
Monitor success rates in performance summaries:
```
[FRAME_TAG] Success rate: 78%
[FRAME_TAG] *** RELIABILITY WARNING *** Download success rate < 90%
```
Low success rates indicate network connectivity or server issues.

### 3. Ad-Related Problems
Check ad segment replacement timing:
```
[FRAME_TAG] Frame #45 [AD_PLACEHOLDER] - Total processed: 45, Ad segments skipped: 8
```
High ad segment counts might indicate ad blocking is affecting stream continuity.

### 4. Performance Trends
Compare average times across summaries:
```
Summary 1: [FRAME_TAG] Download times - Avg: 145ms, Max: 890ms
Summary 2: [FRAME_TAG] Download times - Avg: 2340ms, Max: 5670ms  ← Performance degrading
```

## Performance Thresholds

### Warning Levels
- **Lag Warning**: Average download time > 2000ms (2 seconds)
- **Reliability Warning**: Download success rate < 90%
- **Continuity Warning**: More than 5 sequence gaps detected

### Optimal Performance
- **Download Time**: < 500ms average for smooth playback
- **Success Rate**: > 95% for reliable streaming
- **Max Download Time**: < 2000ms to prevent buffer underruns

## Technical Implementation

### Data Structures
- **Frame Counter**: Sequential numbering for each segment
- **Timing Array**: Rolling window of recent download times (50 entries)
- **Success Tracking**: Processed vs. successfully downloaded segments
- **Gap Detection**: Basic sequence continuity monitoring

### Performance Impact
- **Memory Usage**: Minimal (~2KB per stream for tracking data)
- **CPU Overhead**: Negligible (< 0.1% additional CPU usage)
- **Log Volume**: Moderate increase in debug output

## Integration Notes

Frame Number Tagging is automatically enabled when streaming starts. No configuration is required - the system will:

1. Initialize tracking when the download thread starts
2. Tag each segment with performance data
3. Generate periodic summaries every 30 frames
4. Provide final performance report when streaming ends
5. Issue warnings when performance issues are detected

The feature is designed to be non-intrusive and help diagnose lag issues without affecting streaming performance.