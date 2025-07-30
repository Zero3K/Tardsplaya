#pragma once
//
// Adapted Simple HLS Client - Media Parser for Tardsplaya
// Original from https://github.com/bytems/simple_hls_client
//

#ifndef SIMPLE_HLS_CLIENT_MEDIA_PARSER_H
#define SIMPLE_HLS_CLIENT_MEDIA_PARSER_H

#include "hls_tag_parser.h"

// Tag-specific line & data attributes
struct MediaGroup {
    std::string type;           // Audio, Video, Subtitles, etc.
    std::string id;             // GROUP-ID
    std::string name;           // NAME
    std::string language;       // LANGUAGE
    std::string default_;       // DEFAULT (YES/NO)
    std::string autoselect;     // AUTOSELECT (YES/NO)
    int channel_count = 0;      // Number of audio channels
    std::string uri;            // URI (optional for audio)
    std::string manifest_line;  // Original manifest line
    
    // Helper methods
    bool isDefault() const {
        return default_ == "YES";
    }
    
    bool isAutoselect() const {
        return autoselect == "YES";
    }
    
    // Get display name for UI
    std::string getDisplayName() const {
        std::string display = name;
        if (!language.empty()) {
            display += " (" + language + ")";
        }
        if (channel_count > 0) {
            display += " [" + std::to_string(channel_count) + "ch]";
        }
        return display;
    }
};

// Concrete Media sub-parser
class MediaParser : public HLSTagParserSorter<MediaParser, MediaGroup> {
public:
    std::vector<MediaGroup> audio_tracks_;

    void parse(const std::string& content) override {
        MediaGroup cur_audio_trk;
        std::istringstream ss(content);
        std::string line;

        while (std::getline(ss, line)) {
            if (line.find("#EXT-X-MEDIA:") != std::string::npos) {
                cur_audio_trk = MediaGroup();
                cur_audio_trk.manifest_line = line;

                cur_audio_trk.type = extractAttribute(line, "TYPE");
                cur_audio_trk.uri = extractAttribute(line, "URI");
                cur_audio_trk.id = extractAttribute(line, "GROUP-ID");
                cur_audio_trk.name = extractAttribute(line, "NAME");
                cur_audio_trk.autoselect = extractAttribute(line, "AUTOSELECT");
                cur_audio_trk.default_ = extractAttribute(line, "DEFAULT");
                cur_audio_trk.language = extractAttribute(line, "LANGUAGE");
                
                std::string channelType = extractAttribute(line, "CHANNELS");
                
                // Extract channel count from Channels string
                if (!channelType.empty()) {
                    size_t slash_pos = channelType.find('/');
                    if (slash_pos == std::string::npos) {
                        cur_audio_trk.channel_count = std::stoi(channelType);
                    } else {
                        cur_audio_trk.channel_count = std::stoi(channelType.substr(0, slash_pos));
                    }
                }

                audio_tracks_.emplace_back(cur_audio_trk);
            }
        }
    }

    // provide access to the container
    std::vector<MediaGroup>& getContainer() { return audio_tracks_; }

    // Get audio tracks specifically (for convenience)
    const std::vector<MediaGroup>& getAudioTracks() const {
        return audio_tracks_;
    }

private:
    using ComparisonFunc = std::function<bool(const MediaGroup&, const MediaGroup&)>;

    // Mapping between attributes and their comparators
    const std::unordered_map<SortAttribute, ComparisonFunc> comparisons_ = {
        {SortAttribute::ID, [](const MediaGroup& a, const MediaGroup& b) {
            return a.id < b.id;
        }},
        {SortAttribute::NAME, [](const MediaGroup& a, const MediaGroup& b) {
            return a.name < b.name;
        }},
        {SortAttribute::LANGUAGE, [](const MediaGroup& a, const MediaGroup& b) {
            return a.language < b.language;
        }},
        {SortAttribute::DEFAULT_, [](const MediaGroup& a, const MediaGroup& b) {
            return a.default_ < b.default_;
        }},
        {SortAttribute::AUTOSELECT, [](const MediaGroup& a, const MediaGroup& b) {
            return a.autoselect < b.autoselect;
        }},
        {SortAttribute::CHANNELS, [](const MediaGroup& a, const MediaGroup& b) {
            return a.channel_count < b.channel_count;
        }}
    };

public:
    // provide access to the comparisons map
    const std::unordered_map<SortAttribute, ComparisonFunc>& getComparisons() const {
        return comparisons_;
    }
};

#endif // SIMPLE_HLS_CLIENT_MEDIA_PARSER_H