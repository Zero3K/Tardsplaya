#pragma once
//
// Enhanced Playlist Parser using Simple HLS Client for Tardsplaya
//

#ifndef ENHANCED_PLAYLIST_PARSER_H
#define ENHANCED_PLAYLIST_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "simple_hls_client/m3u8_parser.h"
#include "simple_hls_client/hls_fetcher.h"
#include "tsduck_hls_wrapper.h"

// Forward declaration to avoid circular dependency
struct PlaylistQuality;

// Enhanced quality structure that includes audio track info
struct EnhancedPlaylistQuality {
    std::wstring name;              // Display name (e.g. "1080p", "720p", "audio_only")
    std::wstring url;               // URL to the stream
    int bandwidth = 0;              // Bandwidth in bps
    int resolution_height = 0;      // Height in pixels
    int resolution_width = 0;       // Width in pixels
    std::string codecs;             // Video/audio codecs
    std::string audio_group;        // Associated audio group ID
    bool is_audio_only = false;     // True if this is an audio-only stream
    
    // Convert to wide string for UI compatibility
    std::wstring getBandwidthString() const {
        if (bandwidth > 0) {
            return std::to_wstring(bandwidth / 1000) + L"k";
        }
        return L"";
    }
    
    std::wstring getResolutionString() const {
        if (resolution_height > 0) {
            return std::to_wstring(resolution_width) + L"x" + std::to_wstring(resolution_height);
        }
        return L"";
    }
};

// Audio track structure for UI
struct AudioTrack {
    std::wstring id;                // Group ID
    std::wstring name;              // Display name
    std::wstring language;          // Language code
    bool is_default = false;        // Default selection
    bool auto_select = false;       // Auto-select flag
    int channels = 0;               // Channel count
    std::wstring url;               // URL if available
    
    std::wstring getDisplayName() const {
        std::wstring display = name;
        if (!language.empty()) {
            display += L" (" + language + L")";
        }
        if (channels > 0) {
            display += L" [" + std::to_wstring(channels) + L"ch]";
        }
        return display;
    }
};

// Enhanced parsing results
struct EnhancedPlaylistResult {
    std::vector<EnhancedPlaylistQuality> qualities;
    std::vector<AudioTrack> audio_tracks;
    std::map<std::wstring, std::vector<AudioTrack>> quality_to_audio_tracks;
    bool has_audio_variants = false;
    bool has_iframe_streams = false;
    std::wstring error_message;
};

/**
 * @brief Enhanced M3U8 playlist parser using Simple HLS Client components.
 * 
 * This parser provides comprehensive HLS parsing with support for:
 * - Stream variants with detailed metadata
 * - Audio track selection
 * - I-Frame stream support
 * - Automatic quality sorting and organization
 */
class EnhancedPlaylistParser {
private:
    // Convert std::string to std::wstring
    std::wstring StringToWString(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }

    // Convert std::wstring to std::string
    std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    // Resolve relative URLs
    std::wstring JoinUrl(const std::wstring& base_url, const std::string& rel_url) {
        std::wstring wrel_url = StringToWString(rel_url);
        if (wrel_url.empty() || wrel_url.find(L"http") == 0)
            return wrel_url;
        
        // Basic join: find last '/' in base and append rel_url
        size_t pos = base_url.rfind(L'/');
        if (pos == std::wstring::npos) return wrel_url;
        return base_url.substr(0, pos + 1) + wrel_url;
    }

public:
    /**
     * @brief Parse M3U8 master playlist with enhanced functionality.
     * @param content The M3U8 playlist content as wide string.
     * @param base_url Base URL for resolving relative URLs.
     * @return EnhancedPlaylistResult with parsed data and metadata.
     */
    EnhancedPlaylistResult ParseM3U8MasterPlaylist(
        const std::wstring& content,
        const std::wstring& base_url = L""
    ) {
        EnhancedPlaylistResult result;
        
        try {
            std::string content_str = WStringToString(content);
            
            // Use Simple HLS Client parser
            M3U8Parser parser;
            parser.parse(content_str);
            
            // Process stream variants
            const auto& stream_parser = parser.getStreamParser();
            for (const auto& variant : stream_parser.variants_) {
                EnhancedPlaylistQuality quality;
                
                // Basic info
                quality.bandwidth = variant.bandwidth;
                quality.resolution_height = variant.resolution_height;
                quality.resolution_width = variant.resolution_width;
                quality.codecs = variant.codecs;
                quality.audio_group = variant.audio;
                quality.url = JoinUrl(base_url, variant.uri);
                
                // Generate display name
                if (variant.resolution_height > 0) {
                    quality.name = std::to_wstring(variant.resolution_height) + L"p";
                    if (variant.bandwidth > 0) {
                        quality.name += L" (" + std::to_wstring(variant.bandwidth / 1000) + L"k)";
                    }
                } else if (variant.bandwidth > 0) {
                    quality.name = std::to_wstring(variant.bandwidth / 1000) + L"k";
                } else {
                    quality.name = L"unknown";
                }
                
                result.qualities.push_back(quality);
            }
            
            // Process audio tracks
            const auto& audio_parser = parser.getAudioParser();
            for (const auto& media : audio_parser.audio_tracks_) {
                AudioTrack track;
                track.id = StringToWString(media.id);
                track.name = StringToWString(media.name);
                track.language = StringToWString(media.language);
                track.is_default = media.isDefault();
                track.auto_select = media.isAutoselect();
                track.channels = media.channel_count;
                if (!media.uri.empty()) {
                    track.url = JoinUrl(base_url, media.uri);
                }
                
                result.audio_tracks.push_back(track);
            }
            
            // Set metadata flags
            result.has_audio_variants = !result.audio_tracks.empty();
            result.has_iframe_streams = parser.hasIFrameStreams();
            
            // Sort qualities by resolution (highest first) - simple sort
            std::sort(result.qualities.begin(), result.qualities.end(), 
                [](const EnhancedPlaylistQuality& a, const EnhancedPlaylistQuality& b) {
                    if (a.resolution_height != b.resolution_height) {
                        return a.resolution_height > b.resolution_height; // Higher resolution first
                    }
                    return a.bandwidth > b.bandwidth; // Higher bandwidth first as tiebreaker
                });
            
            // If no stream variants found, try legacy parsing
            if (result.qualities.empty()) {
                result = ParseLegacyPlaylist(content, base_url);
            }
            
        } catch (const std::exception& e) {
            result.error_message = StringToWString(std::string("Parse error: ") + e.what());
            // Try fallback parsing
            result = ParseLegacyPlaylist(content, base_url);
        }
        
        return result;
    }

    /**
     * @brief Filter discontinuity segments from a media playlist.
     * @param playlist_content The M3U8 media playlist content (not master playlist).
     * @param base_url Base URL for resolving relative URLs.
     * @return A clean playlist string with discontinuity segments removed.
     */
    std::string FilterDiscontinuitySegments(
        const std::string& playlist_content,
        const std::string& base_url = ""
    ) {
        try {
            // Use TSDuck HLS wrapper for precise discontinuity detection
            tsduck_hls::PlaylistParser parser;
            if (!parser.ParsePlaylist(playlist_content)) {
                return playlist_content; // Return original if parsing fails
            }
            
            auto segments = parser.GetSegments();
            
            // Filter out segments with discontinuity markers
            std::vector<tsduck_hls::MediaSegment> filtered_segments;
            for (const auto& segment : segments) {
                if (!segment.has_discontinuity) {
                    filtered_segments.push_back(segment);
                }
            }
            
            // Reconstruct playlist without discontinuity segments
            return CreateFilteredPlaylist(playlist_content, filtered_segments);
            
        } catch (const std::exception& e) {
            // Return original playlist if filtering fails
            return playlist_content;
        }
    }

    /**
     * @brief Get filtered media segments without discontinuities.
     * @param playlist_content The M3U8 media playlist content.
     * @return Vector of segments without discontinuity markers.
     */
    std::vector<tsduck_hls::MediaSegment> GetFilteredSegments(
        const std::string& playlist_content
    ) {
        std::vector<tsduck_hls::MediaSegment> filtered_segments;
        
        try {
            tsduck_hls::PlaylistParser parser;
            if (parser.ParsePlaylist(playlist_content)) {
                auto segments = parser.GetSegments();
                for (const auto& segment : segments) {
                    if (!segment.has_discontinuity) {
                        filtered_segments.push_back(segment);
                    }
                }
            }
        } catch (const std::exception&) {
            // Return empty vector if parsing fails
        }
        
        return filtered_segments;
    }

private:
    // Fallback to simple parsing for non-master playlists
    EnhancedPlaylistResult ParseLegacyPlaylist(const std::wstring& content, const std::wstring& base_url) {
        EnhancedPlaylistResult result;
        
        std::wistringstream ss(content);
        std::wstring line;
        
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == L'#')
                continue;
                
            EnhancedPlaylistQuality single;
            single.name = L"default";
            single.url = JoinUrl(base_url, WStringToString(line));
            result.qualities.push_back(single);
            break;
        }
        
        return result;
    }

private:
    /**
     * @brief Create a clean M3U8 playlist from filtered segments.
     * @param original_content Original playlist content for extracting headers.
     * @param filtered_segments Segments without discontinuity markers.
     * @return Clean M3U8 playlist string.
     */
    std::string CreateFilteredPlaylist(
        const std::string& original_content,
        const std::vector<tsduck_hls::MediaSegment>& filtered_segments
    ) {
        std::ostringstream clean_playlist;
        std::istringstream original_stream(original_content);
        std::string line;
        
        // Copy headers and metadata (everything before the first segment)
        bool in_header_section = true;
        while (std::getline(original_stream, line) && in_header_section) {
            // Skip discontinuity tags in headers
            if (line.find("#EXT-X-DISCONTINUITY") == 0) {
                continue;
            }
            
            // Copy header tags
            if (line[0] == '#') {
                if (line.find("#EXTINF:") == 0) {
                    // We've reached the segments section
                    in_header_section = false;
                    break;
                } else {
                    clean_playlist << line << "\n";
                }
            }
        }
        
        // Add filtered segments
        for (const auto& segment : filtered_segments) {
            // Convert duration back to EXTINF format
            double duration_seconds = segment.duration.count() / 1000.0;
            clean_playlist << "#EXTINF:" << std::fixed << std::setprecision(3) 
                          << duration_seconds << ",\n";
            
            // Convert wide string URL back to string
            std::string url_str = WStringToString(segment.url);
            clean_playlist << url_str << "\n";
        }
        
        // Add end list if original had it
        if (original_content.find("#EXT-X-ENDLIST") != std::string::npos) {
            clean_playlist << "#EXT-X-ENDLIST\n";
        }
        
        return clean_playlist.str();
    }
};

// Enhanced playlist parsing with Simple HLS Client integration
inline EnhancedPlaylistResult ParseM3U8MasterPlaylistEnhanced(
    const std::wstring& playlist_content,
    const std::wstring& base_url
) {
    EnhancedPlaylistParser parser;
    return parser.ParseM3U8MasterPlaylist(playlist_content, base_url);
}

// Discontinuity filtering functions for media playlists

/**
 * @brief Filter discontinuity segments from a media playlist.
 * 
 * This function takes an M3U8 media playlist (not master playlist) and removes
 * all segments that are marked with #EXT-X-DISCONTINUITY tags. This is useful
 * for removing ad segments or other content that causes decoder resets.
 * 
 * Example usage:
 * ```cpp
 * // Download media playlist
 * std::string media_playlist = "..."; // M3U8 content from stream URL
 * 
 * // Filter out discontinuity segments (typically ads)
 * std::string clean_playlist = FilterDiscontinuitySegments(media_playlist);
 * 
 * // Use clean_playlist to pipe only main content to media player
 * ```
 * 
 * @param playlist_content The M3U8 media playlist content (not master playlist).
 * @param base_url Base URL for resolving relative URLs (optional).
 * @return A clean playlist string with discontinuity segments removed.
 */
inline std::string FilterDiscontinuitySegments(
    const std::string& playlist_content,
    const std::string& base_url = ""
) {
    EnhancedPlaylistParser parser;
    return parser.FilterDiscontinuitySegments(playlist_content, base_url);
}

/**
 * @brief Get filtered media segments without discontinuities.
 * 
 * Returns a vector of MediaSegment objects that don't have discontinuity markers.
 * Useful for direct access to segment data without reconstructing the playlist.
 * 
 * @param playlist_content The M3U8 media playlist content.
 * @return Vector of segments without discontinuity markers.
 */
inline std::vector<tsduck_hls::MediaSegment> GetFilteredSegments(
    const std::string& playlist_content
) {
    EnhancedPlaylistParser parser;
    return parser.GetFilteredSegments(playlist_content);
}

#endif // ENHANCED_PLAYLIST_PARSER_H