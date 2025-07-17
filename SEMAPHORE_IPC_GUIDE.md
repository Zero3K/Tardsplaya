# Semaphore-based IPC Implementation

## Overview

The Tardsplaya application now includes enhanced Inter-Process Communication (IPC) using Windows counting semaphores to improve stream buffer management and flow control.

## What Was Added

### New Semaphore Classes

1. **StreamSemaphore** - A wrapper around Windows counting semaphores
   - Supports named semaphores for cross-process communication
   - Includes timeout mechanisms for robust operation
   - Tracks approximate count for debugging purposes

2. **ProducerConsumerSemaphores** - Specialized semaphore pair for buffer management
   - `empty_slots` semaphore tracks available buffer space
   - `filled_slots` semaphore tracks items ready for consumption
   - Implements classic producer-consumer pattern

3. **StreamSemaphoreUtils** - Utility functions for stream-specific semaphores
   - Generates unique semaphore names per stream
   - Creates configured semaphore pairs for streaming

### Integration Points

The semaphore-based IPC is integrated into the streaming pipeline:

- **Producer Thread** (download): Waits for buffer slots before adding segments
- **Consumer Thread** (feeder): Waits for available items before consuming
- **Flow Control**: Prevents buffer overflow and underflow conditions
- **Graceful Fallback**: Falls back to mutex-only operation if semaphores fail

## How It Improves IPC

### Before (Mutex-only approach)
```cpp
// Old approach - potential for buffer overflow
{
    std::lock_guard<std::mutex> lock(buffer_mutex);
    buffer_queue.push(std::move(segment_data)); // Could overflow
}
```

### After (Semaphore-based approach)
```cpp
// New approach - controlled flow with semaphores
if (buffer_semaphores->WaitForProduceSlot(5000)) { // Wait for space
    {
        std::lock_guard<std::mutex> lock(buffer_mutex);
        buffer_queue.push(std::move(segment_data));
    }
    buffer_semaphores->SignalItemProduced(); // Signal item available
}
```

## Benefits

1. **Buffer Overflow Prevention**: Producer waits for available slots
2. **Better Flow Control**: Consumer waits for actual data availability  
3. **Resource Management**: Prevents unlimited buffer growth
4. **Multi-Stream Support**: Each stream gets its own semaphore set
5. **Timeout Handling**: Prevents indefinite blocking on failures
6. **Cross-Process IPC**: Named semaphores can communicate between processes
7. **Performance**: Reduces busy-waiting and improves efficiency

## Configuration

Semaphore-based IPC can be enabled/disabled per stream:

```cpp
bool success = BufferAndPipeStreamToPlayer(
    player_path, playlist_url, cancel_token, buffer_segments,
    channel_name, chunk_count, selected_quality, 
    true  // use_semaphore_ipc - enables semaphore flow control
);
```

## Debugging

The implementation includes comprehensive logging:
- Semaphore creation and destruction
- Producer/consumer operations
- Timeout events and fallback scenarios
- Buffer status and flow control events

## Backward Compatibility

The implementation maintains full backward compatibility:
- Default behavior enables semaphore IPC
- Automatic fallback to mutex-only approach on semaphore failures
- No changes to existing API calls
- Graceful degradation ensures streams continue working

This enhancement addresses the original issue by implementing proper semaphore-based IPC for better stream management and resource control.