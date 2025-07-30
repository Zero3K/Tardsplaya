# Simple HLS Client Integration

This document describes the integration of the Simple HLS Client library into Tardsplaya for enhanced HLS playlist parsing capabilities.

## Overview

The Simple HLS Client (https://github.com/bytems/simple_hls_client) provides comprehensive M3U8 playlist parsing with support for advanced HLS features. This integration replaces the basic playlist parser with a full-featured implementation.

## Features Added

### Enhanced M3U8 Parsing
- **Stream Variants**: Comprehensive parsing of `#EXT-X-STREAM-INF` tags with detailed metadata
- **Audio Tracks**: Support for `#EXT-X-MEDIA` tags enabling audio track selection
- **I-Frame Streams**: Parsing of `#EXT-X-I-FRAME-STREAM-INF` tags for seek preview support
- **Sorting**: Advanced sorting capabilities by bandwidth, resolution, language, and other attributes

### Audio Track Selection
- New UI component for selecting alternative audio tracks
- Automatic detection and display of available audio languages
- Default track selection based on stream metadata
- Channel count and language information display

### Windows Integration
- Native WinHTTP implementation replacing libcurl dependency
- Fallback to custom TLS client for maximum compatibility
- Header-only design for easy maintenance and deployment

## Architecture

### Core Components

#### Simple HLS Client Headers (`simple_hls_client/`)
- `hls_tag_parser.h` - Base parser class with CRTP design pattern
- `stream_inf_parser.h` - Video stream variant parsing
- `media_parser.h` - Audio track parsing 
- `iframe_parser.h` - I-Frame stream parsing
- `m3u8_parser.h` - Main parser coordinating all sub-parsers
- `hls_fetcher.h` - Windows HTTP client with TLS fallback

#### Enhanced Playlist Parser (`enhanced_playlist_parser.h`)
- Windows-compatible wrapper around Simple HLS Client
- Provides wide string (std::wstring) API for UI compatibility
- Maintains backward compatibility with existing code
- Automatic URL resolution for relative paths

### Integration Points

#### UI Components
- **Quality Selection**: Enhanced with bandwidth and resolution metadata
- **Audio Track Selection**: New listbox for choosing audio languages/variants
- **Status Display**: Shows audio track information in logs

#### API Compatibility
- `ParseM3U8MasterPlaylist()` - Backward compatible function
- `ParseM3U8MasterPlaylistEnhanced()` - New function with full feature access
- Existing code continues to work without modification

## Usage

### Basic Quality Parsing (Existing Code)
```cpp
std::vector<PlaylistQuality> qualities = ParseM3U8MasterPlaylist(playlist_content, base_url);
```

### Enhanced Parsing with Audio Tracks
```cpp
EnhancedPlaylistResult result = ParseM3U8MasterPlaylistEnhanced(playlist_content, base_url);

// Access stream qualities
for (const auto& quality : result.qualities) {
    std::wcout << quality.name << L" - " << quality.getBandwidthString() << std::endl;
}

// Access audio tracks
for (const auto& track : result.audio_tracks) {
    std::wcout << track.getDisplayName() << std::endl;
}
```

### Sorting and Organization
```cpp
M3U8Parser parser;
parser.parse(content);

// Sort streams by resolution, then bandwidth
auto streamAccessor = parser.select<ParserType::STREAM>();
streamAccessor.sort(HLSTagParser::SortAttribute::RESOLUTION, 
                   HLSTagParser::SortAttribute::BANDWIDTH);

// Sort audio tracks by language
auto audioAccessor = parser.select<ParserType::AUDIO>();
audioAccessor.sort(HLSTagParser::SortAttribute::LANGUAGE);
```

## Benefits

### For Users
- **Audio Language Selection**: Choose preferred audio language for multilingual streams
- **Better Quality Information**: Detailed bandwidth and resolution data
- **Improved Compatibility**: Better parsing of complex HLS playlists

### For Developers
- **Extensible Design**: Easy to add support for new HLS tags
- **Clean Architecture**: Separation of concerns with dedicated parsers
- **Modern C++**: Template-based design with compile-time optimization
- **No Dependencies**: Header-only implementation with Windows native HTTP

## Testing

The integration includes comprehensive testing:

1. **Compilation Testing**: Verifies header compatibility and syntax
2. **Functionality Testing**: Tests parsing of various HLS playlist formats
3. **UI Integration Testing**: Ensures proper display of audio tracks
4. **Backward Compatibility**: Existing functionality remains unchanged

## Future Enhancements

The modular design enables future additions:
- **Subtitle Track Support**: Easy to add with new parser
- **DRM Content Support**: Framework ready for content protection
- **Custom Tag Support**: Extensible parser architecture
- **Advanced Filtering**: Sort and filter by custom criteria