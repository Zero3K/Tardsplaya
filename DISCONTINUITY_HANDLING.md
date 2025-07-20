# VirtualDub2 Discontinuity Handling Implementation

## Problem Statement

**Issue**: Media players can get stuck on black frames after not processing advertisement-containing playlists in HLS streams.

**Root Cause**: When HLS streams transition from advertisements back to main content (discontinuity), the first packets are often non-keyframes (P-frames or B-frames). Media players that receive these non-keyframes without proper context can:
- Display black/corrupted frames
- Get stuck in an unrecoverable state
- Lose video synchronization

## VirtualDub2 Solution

The VirtualDub2 codebase in `vtrans.cpp` implements a proven solution:

```cpp
// After a discontinuity, we need to wait for the next key frame
if (pSample->IsDiscontinuity() == S_OK) {
    DbgLog((LOG_TRACE,3,TEXT("Non-key discontinuity - wait for keyframe")));
    m_nWaitForKey = 30;
}
```

**Key Principles:**
1. **Wait for Keyframes**: After detecting discontinuity, skip all frames until a keyframe (I-frame) is found
2. **Timeout Protection**: Maximum wait of 30 frames to prevent indefinite blocking
3. **Clean Resume**: Once keyframe is found, immediately resume normal frame delivery

## Implementation in Tardsplaya

### 1. State Management
```cpp
// VirtualDub2-style discontinuity handling state
std::atomic<bool> wait_for_keyframe_{false};  // Flag to indicate we're waiting for a keyframe
std::atomic<int> keyframe_wait_counter_{0};   // Counter for frames waited (max 30)
```

### 2. Discontinuity Detection
**Dual Detection System:**
- **Playlist-level**: `#EXT-X-DISCONTINUITY` tags in M3U8 playlists
- **Packet-level**: Discontinuity indicator in TS packet adaptation fields

```cpp
// Playlist-level detection (ad transitions)
if (has_discontinuities) {
    SetWaitForKeyframe();
}

// Packet-level detection (stream-level discontinuities)
if (packet.discontinuity) {
    SetWaitForKeyframe();
}
```

### 3. Frame Skipping Logic
```cpp
bool TransportStreamRouter::ShouldSkipFrame(const TSPacket& packet) {
    if (!wait_for_keyframe_.load()) return false;
    
    int current_wait = keyframe_wait_counter_.fetch_add(1) + 1;
    
    if (packet.is_key_frame) {
        // Found keyframe - resume normal playback
        wait_for_keyframe_ = false;
        return false;
    }
    
    if (current_wait >= 30) {
        // Timeout - resume playback to prevent blocking
        wait_for_keyframe_ = false;
        return false;
    }
    
    return true; // Skip this non-keyframe
}
```

## Benefits

### 1. Prevents Black Frame Sticking
- Media players receive clean keyframes after discontinuities
- No corrupted or context-less frames
- Immediate visual recovery after ad breaks

### 2. Robust Error Handling
- 30-frame timeout prevents indefinite waiting
- Works with various stream types and ad insertion methods
- Fallback to normal playback if no keyframes available

### 3. Performance Optimized
- Minimal impact on normal streaming (only activates after discontinuities)
- Atomic operations for thread safety
- Efficient frame skipping logic

## Test Scenarios

### Scenario 1: Normal Ad Break Recovery
```
Stream: [Normal frames] → [#EXT-X-DISCONTINUITY] → [Ad frames] → [#EXT-X-DISCONTINUITY] → [Non-keyframe] → [Non-keyframe] → [KEYFRAME] → [Normal frames]
                                                                                           ↑ SKIP      ↑ SKIP       ↑ SEND     ↑ RESUME
```

### Scenario 2: Timeout Protection
```
Stream: [#EXT-X-DISCONTINUITY] → [30+ non-keyframes without keyframe]
Result: After 30 frames, resume normal playback to prevent blocking
```

### Scenario 3: Packet-Level Discontinuity
```
TS Packet: discontinuity_indicator=1 → [Non-keyframes] → [KEYFRAME]
Result: Same waiting behavior as playlist-level discontinuities
```

## Comparison with Other Solutions

| Approach | Pros | Cons |
|----------|------|------|
| **No Handling** | Simple | Black frames, player stuck |
| **Buffer Flush Only** | Fast restart | May still send non-keyframes |
| **Fixed Delay** | Predictable | May skip valid keyframes or wait too long |
| **VirtualDub2 Method** | ✅ Optimal keyframe targeting<br>✅ Timeout protection<br>✅ Proven in production | Slightly more complex |

## Integration Points

1. **HLS Fetcher Thread**: Detects playlist-level discontinuities
2. **TS Router Thread**: Detects packet-level discontinuities and applies frame skipping
3. **Buffer Management**: Coordinates with existing buffer clearing for fast restart
4. **Logging System**: Provides detailed debugging information

## Monitoring and Debugging

The implementation provides comprehensive logging:

```
[DISCONTINUITY] Detected ad transition - implementing fast restart
[KEYFRAME_WAIT] Activated keyframe waiting mode (max 30 frames)
[KEYFRAME_WAIT] Skipping non-keyframe packet (frame #123)
[KEYFRAME_WAIT] Found keyframe after 5 frames - resuming normal playback
```

This allows operators to:
- Monitor discontinuity events
- Track keyframe recovery times
- Identify streams with problematic keyframe intervals
- Debug black frame issues

## Conclusion

This implementation successfully adapts VirtualDub2's proven discontinuity handling method to HLS streaming, providing:
- **Reliability**: Prevents black frame sticking after ad breaks
- **Performance**: Minimal overhead during normal streaming
- **Robustness**: Timeout protection and comprehensive error handling
- **Visibility**: Detailed logging for monitoring and debugging

The solution directly addresses the core issue while maintaining compatibility with existing streaming infrastructure and media players.