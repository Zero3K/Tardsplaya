#pragma once
#include <string>
#include <vector>
#include "enhanced_playlist_parser.h"

// Represents a stream quality entry in the playlist
struct PlaylistQuality {
    std::wstring name;    // e.g. "1080p (source)", "720p", "audio_only"
    std::wstring url;     // Absolute or relative URL to the stream
};

// Parses a master M3U8 playlist and extracts available qualities and their URLs.
// Returns a vector of PlaylistQuality. If base_url is provided, relative URLs will be resolved.
std::vector<PlaylistQuality> ParseM3U8MasterPlaylist(
    const std::wstring& playlist_content,
    const std::wstring& base_url = L""
);

// Enhanced playlist parsing with Simple HLS Client integration
EnhancedPlaylistResult ParseM3U8MasterPlaylistEnhanced(
    const std::wstring& playlist_content,
    const std::wstring& base_url = L""
);