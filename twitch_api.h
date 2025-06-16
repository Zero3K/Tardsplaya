#pragma once
#include <string>
#include <vector>

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