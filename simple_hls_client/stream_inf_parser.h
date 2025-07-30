#pragma once
//
// Adapted Simple HLS Client - Stream Info Parser for Tardsplaya
// Original from https://github.com/bytems/simple_hls_client
//

#ifndef SIMPLE_HLS_CLIENT_STREAM_INF_PARSER_H
#define SIMPLE_HLS_CLIENT_STREAM_INF_PARSER_H

#include "hls_tag_parser.h"

struct VideoStreamVariant {
    int bandwidth = 0;
    int avg_bandwidth = 0;
    std::string codecs;
    int resolution_height = 0;
    int resolution_width = 0;
    std::string frame_rate;
    std::string video_range;
    std::string audio;
    std::string closed_captions;
    std::string uri;
    std::string manifest_line;
    
    // Helper to get resolution as string
    std::string getResolutionString() const {
        if (resolution_width > 0 && resolution_height > 0) {
            return std::to_string(resolution_width) + "x" + std::to_string(resolution_height);
        }
        return "";
    }
    
    // Helper to get quality name (for compatibility with existing code)
    std::string getQualityName() const {
        if (resolution_height > 0) {
            return std::to_string(resolution_height) + "p";
        } else if (bandwidth > 0) {
            return std::to_string(bandwidth / 1000) + "k";
        }
        return "unknown";
    }
};

// Concrete Video Variation sub-parser
class StreamInfParser : public HLSTagParserSorter<StreamInfParser, VideoStreamVariant> {
public:
    std::vector<VideoStreamVariant> variants_;

    void parse(const std::string& content) override {
        std::istringstream ss(content);
        std::string line;

        VideoStreamVariant current_variant;
        bool expecting_uri = false;

        while (std::getline(ss, line)) {
            if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                current_variant = VideoStreamVariant();
                current_variant.manifest_line = line;

                // Extract bandwidth
                std::string bandwidth_str = extractAttribute(line, "BANDWIDTH");
                if (!bandwidth_str.empty()) {
                    current_variant.bandwidth = std::stoi(bandwidth_str);
                }

                // Extract average bandwidth
                std::string avg_bandwidth_str = extractAttribute(line, "AVERAGE-BANDWIDTH");
                if (!avg_bandwidth_str.empty()) {
                    current_variant.avg_bandwidth = std::stoi(avg_bandwidth_str);
                }

                current_variant.codecs = extractAttribute(line, "CODECS");
                
                // Extract resolution
                std::string resolution = extractAttribute(line, "RESOLUTION");
                if (!resolution.empty()) {
                    size_t x_pos = resolution.find('x');
                    if (x_pos != std::string::npos) {
                        current_variant.resolution_width = std::stoi(resolution.substr(0, x_pos));
                        current_variant.resolution_height = std::stoi(resolution.substr(x_pos + 1));
                    }
                }

                current_variant.frame_rate = extractAttribute(line, "FRAME-RATE");
                current_variant.video_range = extractAttribute(line, "VIDEO-RANGE");
                current_variant.audio = extractAttribute(line, "AUDIO");
                current_variant.closed_captions = extractAttribute(line, "CLOSED-CAPTIONS");

                expecting_uri = true;
            }
            else if (expecting_uri && !line.empty() && line[0] != '#') {
                current_variant.uri = line;
                variants_.emplace_back(current_variant);
                expecting_uri = false;
            }
        }
    }

    // provide access to the container
    std::vector<VideoStreamVariant>& getContainer() { return variants_; }

private:
    using ComparisonFunc = std::function<bool(const VideoStreamVariant&, const VideoStreamVariant&)>;
    
    // Mapping between attributes and their comparators
    const std::unordered_map<SortAttribute, ComparisonFunc> comparisons_ = {
        {SortAttribute::BANDWIDTH, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.bandwidth < b.bandwidth;
        }},
        {SortAttribute::AVERAGE_BANDWIDTH, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.avg_bandwidth < b.avg_bandwidth;
        }},
        {SortAttribute::CODECS, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.codecs < b.codecs;
        }},
        {SortAttribute::RESOLUTION, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.resolution_height < b.resolution_height;
        }},
        {SortAttribute::FRAME_RATE, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.frame_rate < b.frame_rate;
        }},
        {SortAttribute::VIDEO_RANGE, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.video_range < b.video_range;
        }},
        {SortAttribute::AUDIO, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.audio < b.audio;
        }},
        {SortAttribute::CLOSED_CAPTIONS, [](const VideoStreamVariant& a, const VideoStreamVariant& b) {
            return a.closed_captions < b.closed_captions;
        }}
    };

public:
    // provide access to the comparisons map
    const std::unordered_map<SortAttribute, ComparisonFunc>& getComparisons() const {
        return comparisons_;
    }
};

#endif // SIMPLE_HLS_CLIENT_STREAM_INF_PARSER_H