# Tardsplaya - Twitch Stream Buffer

A C++ application that buffers Twitch streams to media players like MPC-HC, MPC-BE, VLC, and MPV.

## Features

- **Stream Buffering**: Downloads and buffers stream segments before sending to media player for smooth playback
- **Multiple Quality Support**: Automatically detects and allows selection of available stream qualities
- **Multi-Stream Support**: Open multiple tabs to watch different streams simultaneously
- **Windows 7 Compatibility**: Includes certificate validation workarounds for older Windows versions
- **Real-time Logging**: Shows detailed logs of streaming operations
- **Modern C++ Implementation**: Clean, efficient C++17 code with minimal dependencies

## Requirements

- Windows 7 or later
- Media player (MPV recommended, also supports VLC, MPC-HC, MPC-BE)
- Internet connection

## Usage

1. **Launch the application**
2. **Enter a Twitch channel name** in the Channel field
3. **Click "Load"** to fetch available stream qualities
4. **Select a quality** from the dropdown list
5. **Click "Watch"** to start buffered streaming
6. **Click "Stop"** to stop the stream

### Multiple Streams

- Use **File → New Stream** (Ctrl+T) to open additional tabs
- Use **File → Close Active Stream** (Ctrl+W) to close the current tab
- Use **File → Close All Streams** to close all tabs

## Configuration

The application defaults to using MPV as the media player. To use a different player:

1. Go to **Tools → Settings** (when implemented)
2. Set the player path and arguments

Default settings:
- Player: `mpv.exe`
- Arguments: `-` (reads from stdin)

## Technical Improvements

This C++ version includes several improvements over the original:

### Windows 7 Compatibility
- Certificate validation bypass for HTTPS requests
- Compatible with older root certificate stores

### Modern API Usage
- Updated Twitch API endpoints
- Proper error handling and retry logic
- Thread-safe logging system

### Memory Efficiency
- Minimal memory footprint
- Efficient stream buffering
- Clean resource management

### Code Quality
- Removed deprecated `<codecvt>` header usage
- Modern C++17 features
- Comprehensive error handling

## Building

Requirements:
- Visual Studio 2019 or later
- Windows SDK 10.0

Build steps:
1. Open `Tardsplaya.sln` in Visual Studio
2. Select Release configuration
3. Build → Build Solution

## License

GNU General Public License v3.0

## Contributing

Feel free to submit issues and pull requests to improve the application.
