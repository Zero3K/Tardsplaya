#pragma once
#include <string>
#include <vector>
#include <map>

// Forward declaration for debug logging function
void AddLog(const std::wstring& msg);

// Output: qualities - list of available qualities (e.g. "1080p (source)", "720p", etc.)
// Output: playlist_url - the best playlist URL (for the selected quality)
// Output: log - (optional) log lines for debugging
//
// Returns true if stream found, false otherwise.
bool FetchTwitchStreamQualities(
    const std::wstring& channel,
    std::vector<std::wstring>& qualities,
    std::wstring& playlist_url,
    std::vector<std::wstring>* log = nullptr
);

// Modern GraphQL API approach for getting stream access token
// Returns the access token in format "signature|token" or empty string on failure
std::wstring GetModernAccessToken(const std::wstring& channel);

// Generate VOD playlist URL for a channel (alternative to live HLS when discontinuities found)
// Returns empty string if unable to generate VOD URL
std::wstring GenerateVodPlaylistUrl(const std::wstring& channel);

// Parse M3U8 playlist using improved logic from TLS client example
// Returns a map of quality names to stream URLs
std::map<std::wstring, std::wstring> ParseM3U8Playlist(const std::string& m3u8Content);