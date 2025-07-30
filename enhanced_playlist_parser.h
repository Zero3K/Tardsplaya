#pragma once
//
// Enhanced Playlist Parser using Simple HLS Client for Tardsplaya
//

#ifndef ENHANCED_PLAYLIST_PARSER_H
#define ENHANCED_PLAYLIST_PARSER_H

#include <string>
#include <vector>
#include <map>
#include "simple_hls_client/m3u8_parser.h"
#include "simple_hls_client/hls_fetcher.h"

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
                quality.audio_group = StringToWString(variant.audio);
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
            
            // Sort qualities by resolution (highest first)
            auto stream_accessor = const_cast<M3U8Parser&>(parser).select<ParserType::STREAM>();
            stream_accessor.sort(HLSTagParser::SortAttribute::RESOLUTION, HLSTagParser::SortAttribute::BANDWIDTH);
            
            // Re-extract sorted qualities
            result.qualities.clear();
            for (const auto& variant : stream_parser.variants_) {
                EnhancedPlaylistQuality quality;
                quality.bandwidth = variant.bandwidth;
                quality.resolution_height = variant.resolution_height;
                quality.resolution_width = variant.resolution_width;
                quality.codecs = variant.codecs;
                quality.audio_group = StringToWString(variant.audio);
                quality.url = JoinUrl(base_url, variant.uri);
                
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
            
            // Reverse to get highest quality first
            std::reverse(result.qualities.begin(), result.qualities.end());
            
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
};

// Backward compatibility function for existing code
inline std::vector<PlaylistQuality> ParseM3U8MasterPlaylist(
    const std::wstring& playlist_content,
    const std::wstring& base_url = L""
) {
    EnhancedPlaylistParser parser;
    auto result = parser.ParseM3U8MasterPlaylist(playlist_content, base_url);
    
    // Convert to legacy format
    std::vector<PlaylistQuality> legacy_result;
    for (const auto& quality : result.qualities) {
        PlaylistQuality legacy_quality;
        legacy_quality.name = quality.name;
        legacy_quality.url = quality.url;
        legacy_result.push_back(legacy_quality);
    }
    
    return legacy_result;
}

#endif // ENHANCED_PLAYLIST_PARSER_H