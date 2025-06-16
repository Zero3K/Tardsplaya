#include "playlist_parser.h"
#include <sstream>

// Helper: resolve relative URL
static std::wstring JoinUrl(const std::wstring& base_url, const std::wstring& rel_url) {
    if (rel_url.empty() || rel_url.find(L"http") == 0)
        return rel_url;
    // Basic join: find last '/' in base and append rel_url
    size_t pos = base_url.rfind(L'/');
    if (pos == std::wstring::npos) return rel_url;
    return base_url.substr(0, pos + 1) + rel_url;
}

std::vector<PlaylistQuality> ParseM3U8MasterPlaylist(
    const std::wstring& content,
    const std::wstring& base_url
) {
    std::vector<PlaylistQuality> result;
    std::wistringstream ss(content);
    std::wstring line, last_inf, last_url;
    PlaylistQuality qual;

    while (std::getline(ss, line)) {
        if (line.empty())
            continue;
        if (line.find(L"#EXT-X-STREAM-INF:") == 0) {
            // Try to extract NAME="..."
            size_t name_pos = line.find(L"NAME=\"");
            if (name_pos != std::wstring::npos) {
                auto rest = line.substr(name_pos + 6);
                auto end = rest.find(L"\"");
                qual.name = rest.substr(0, end);
            } else {
                // Try to extract RESOLUTION, e.g. RESOLUTION=1920x1080
                size_t res_pos = line.find(L"RESOLUTION=");
                if (res_pos != std::wstring::npos) {
                    auto rest = line.substr(res_pos + 11);
                    auto end = rest.find_first_of(L",\r\n");
                    qual.name = rest.substr(0, end);
                } else {
                    qual.name = L"unknown";
                }
            }
            last_inf = line;
        } else if (line[0] != L'#') {
            // This is a URL, following an EXT-X-STREAM-INF
            qual.url = JoinUrl(base_url, line);
            if (qual.name.empty())
                qual.name = qual.url;
            result.push_back(qual);
            qual = PlaylistQuality();  // reset for next
        }
    }

    // Audio-only or single playlist (not master)
    if (result.empty()) {
        // If it's just a .ts or .aac or similar, treat as a single entry
        std::wistringstream ss2(content);
        while (std::getline(ss2, line)) {
            if (line.empty() || line[0] == L'#')
                continue;
            PlaylistQuality single;
            single.name = L"default";
            single.url = JoinUrl(base_url, line);
            result.push_back(single);
            break;
        }
    }

    return result;
}