# TSReadEX Integration for Tardsplaya

## Overview

[TSReadEX](https://github.com/xtne6f/tsreadex) is a powerful MPEG-TS stream processing tool that can enhance Tardsplaya's transport stream capabilities. This document explains how TSReadEX can be integrated and used with Tardsplaya.

## TSReadEX Capabilities

TSReadEX provides advanced MPEG-TS processing features that complement Tardsplaya's existing transport stream router:

### Core Features
- **Stream Selection**: Filter specific services/programs from multi-program streams
- **PID Filtering**: Remove unwanted PIDs (e.g., EIT, subtitles, extra audio tracks)
- **Service Stabilization**: Ensure consistent stream structure for media players
- **Rate Limiting**: Control read speed to prevent system overload

### Audio Processing
- **Dual-Mono Separation**: Split dual-mono AAC streams into separate stereo tracks
- **Audio Stream Completion**: Add missing audio streams to ensure consistent PMT
- **Multi-Language Support**: Better handling of multi-language content
- **Format Support**: AAC-LC and MP2 audio stream processing

### Advanced Features
- **ARIB Caption Support**: Process Japanese digital TV captions and subtitles
- **Stream Repair**: Fix incomplete or malformed transport streams
- **ID3 Metadata**: Convert ARIB captions to ffmpeg-compatible ID3 timed-metadata
- **Packet Format Support**: Handle 188, 192, and 204-byte TS packets

## Integration Benefits for Tardsplaya

### Enhanced Stream Compatibility
- Better handling of problematic stream sources
- Improved compatibility with various media players
- More stable playback for multi-program streams

### Performance Optimization
- Remove unnecessary data streams to reduce bandwidth
- Rate limiting prevents system overload during live streaming
- Cleaner streams reduce media player processing overhead

### Advanced Features
- Support for Japanese digital TV content (ARIB captions)
- Better multi-language audio handling
- Professional broadcast stream compatibility

## Potential Use Cases

### 1. Stream Cleaning
Remove unwanted elements from transport streams:
```
tsreadex -x 18/38/39 input.m2t > cleaned.m2t
```
- Removes EIT (Electronic Program Guide) data
- Reduces stream size and processing overhead

### 2. Service Selection
Extract specific services from multi-program streams:
```
tsreadex -n -1 -b 1 -c 1 multi_service.m2t > single_service.m2t
```
- Selects first service from PAT
- Ensures second audio track exists
- Ensures captions are present

### 3. Audio Processing
Handle dual-mono and multi-language audio:
```
tsreadex -n 100 -a 9 -b 1 input.m2t > processed.m2t
```
- Selects service ID 100
- Separates dual-mono into stereo + second audio
- Adds missing second audio track

### 4. Rate-Limited Processing
Prevent system overload during live streaming:
```
tsreadex -l 8000 -t 30 live_stream.m2t > buffered.m2t
```
- Limits read speed to 8MB/s
- 30-second timeout for inactive streams

## Implementation in Tardsplaya

### Integration Points

TSReadEX can be integrated into Tardsplaya's transport stream pipeline at several points:

1. **Pre-Processing**: Process HLS segments before TSDuck conversion
2. **Post-Processing**: Clean up generated transport streams
3. **Alternative Router**: Use as alternative to TSDuck for specific use cases
4. **Stream Repair**: Fix problematic streams before playback

### Configuration Options

Proposed configuration structure for TSReadEX integration:

```cpp
struct TSReadEXConfig {
    bool enabled = false;                    // Enable TSReadEX processing
    std::vector<int> exclude_pids;           // PIDs to exclude (-x option)
    int program_selection = 0;               // Program number/index (-n option)  
    int audio1_mode = 0;                     // First audio processing (-a option)
    int audio2_mode = 0;                     // Second audio processing (-b option)
    int caption_mode = 0;                    // Caption processing (-c option)
    int superimpose_mode = 0;                // Superimpose processing (-u option)
    int rate_limit_kbps = 0;                 // Rate limiting (-l option)
    int timeout_seconds = 0;                 // Timeout (-t option)
    bool enable_arib_conversion = false;     // ARIB to ID3 conversion (-d option)
};
```

### Usage Examples

#### Basic Stream Cleaning
```cpp
TSReadEXConfig config;
config.enabled = true;
config.exclude_pids = {0x12, 0x26, 0x27}; // Remove EIT, subtitles
router.SetTSReadEXConfig(config);
```

#### Multi-Language Audio Setup
```cpp
TSReadEXConfig config;
config.enabled = true;
config.program_selection = -1;  // First program in PAT
config.audio1_mode = 1;         // Ensure first audio exists
config.audio2_mode = 3;         // Copy first audio if second doesn't exist
router.SetTSReadEXConfig(config);
```

#### Japanese Broadcasting
```cpp
TSReadEXConfig config;
config.enabled = true;
config.program_selection = -1;
config.caption_mode = 1;            // Ensure captions exist
config.enable_arib_conversion = true; // Convert to ID3 for ffmpeg
router.SetTSReadEXConfig(config);
```

## Technical Implementation

### Windows Integration
- TSReadEX is available as both executable and source code
- Can be integrated as:
  - External process (executable wrapper)
  - Compiled library (integrate source directly)
  - DLL/shared library (if available)

### Process Pipeline
```
HLS Segments → [TSReadEX Processing] → TSDuck Router → Media Player
                      ↓
              Advanced filtering, audio processing, 
              caption conversion, stream repair
```

### Error Handling
- Fallback to standard TSDuck processing if TSReadEX fails
- Graceful degradation for unsupported features
- Logging for troubleshooting integration issues

## Performance Considerations

### Memory Usage
- TSReadEX processes streams in chunks
- Memory usage scales with buffer size settings
- Consider memory constraints for multiple simultaneous streams

### CPU Impact
- Audio processing (dual-mono separation) is CPU-intensive
- Caption conversion adds processing overhead
- Rate limiting can help manage CPU load

### Latency
- Additional processing introduces latency
- Consider real-time requirements for live streaming
- Buffer management affects overall latency

## Licensing and Distribution

### TSReadEX License
- TSReadEX is released under MIT license
- Compatible with Tardsplaya's GPL v3.0 license
- Source code integration is permitted

### Distribution Options
1. **Bundled Executable**: Include pre-compiled TSReadEX binary
2. **Source Integration**: Compile TSReadEX code directly into Tardsplaya
3. **Optional Component**: Download TSReadEX separately as needed

## Conclusion

TSReadEX provides significant enhancements to Tardsplaya's transport stream processing capabilities. Key benefits include:

- **Enhanced Compatibility**: Better handling of diverse stream sources
- **Professional Features**: ARIB caption support, advanced audio processing
- **Performance Control**: Rate limiting and stream optimization
- **Stream Quality**: Repair and stabilization of problematic streams

The integration would make Tardsplaya more robust and capable of handling professional broadcast content while maintaining its ease of use for general streaming applications.

## References

- [TSReadEX GitHub Repository](https://github.com/xtne6f/tsreadex)
- [ARIB STD-B24 (Caption Standard)](http://www.arib.or.jp/english/html/overview/doc/2-STD-B24v5_6-E2.pdf)
- [MPEG-TS Standard (ISO/IEC 13818-1)](https://www.iso.org/standard/74427.html)