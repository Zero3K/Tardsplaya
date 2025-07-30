#pragma once
//
// Adapted Simple HLS Client - I-Frame Parser for Tardsplaya
// Original from https://github.com/bytems/simple_hls_client
//

#ifndef SIMPLE_HLS_CLIENT_IFRAME_PARSER_H
#define SIMPLE_HLS_CLIENT_IFRAME_PARSER_H

#include "hls_tag_parser.h"

// Tag-specific line & data attributes
struct IFrame {
    int bandwidth = 0;
    std::string codecs;
    int resolution_height = 0;
    int resolution_width = 0;
    std::string video_range;
    std::string uri;
    std::string manifest_line;
    
    // Helper to get resolution as string
    std::string getResolutionString() const {
        if (resolution_width > 0 && resolution_height > 0) {
            return std::to_string(resolution_width) + "x" + std::to_string(resolution_height);
        }
        return "";
    }
};

// Concrete I-Frame sub-parser
class iFrameParser : public HLSTagParserSorter<iFrameParser, IFrame> {
public:
    std::vector<IFrame> iframes_;

    void parse(const std::string& content) override {
        IFrame cur_frame;
        std::istringstream iss(content);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.find("#EXT-X-I-FRAME-STREAM-INF") != std::string::npos) {
                cur_frame = IFrame();
                cur_frame.manifest_line = line;

                std::string bandwidth_str = extractAttribute(line, "BANDWIDTH");
                if (!bandwidth_str.empty()) {
                    cur_frame.bandwidth = std::stoi(bandwidth_str);
                }

                cur_frame.uri = extractAttribute(line, "URI");
                cur_frame.video_range = extractAttribute(line, "VIDEO-RANGE");
                cur_frame.codecs = extractAttribute(line, "CODECS");

                // Extract resolution from RESOLUTION attribute
                std::string resolution = extractAttribute(line, "RESOLUTION");
                if (!resolution.empty()) {
                    size_t x_pos = resolution.find('x');
                    if (x_pos != std::string::npos) {
                        cur_frame.resolution_width = std::stoi(resolution.substr(0, x_pos));
                        cur_frame.resolution_height = std::stoi(resolution.substr(x_pos + 1));
                    }
                }

                iframes_.emplace_back(cur_frame);
            }
        }
    }

    // provide access to the container
    std::vector<IFrame>& getContainer() { return iframes_; }

private:
    using ComparisonFunc = std::function<bool(const IFrame&, const IFrame&)>;
    
    // Mapping between attributes and their comparators
    const std::unordered_map<SortAttribute, ComparisonFunc> comparisons_ = {
        {SortAttribute::BANDWIDTH, [](const IFrame& a, const IFrame& b) {
            return a.bandwidth < b.bandwidth;
        }},
        {SortAttribute::CODECS, [](const IFrame& a, const IFrame& b) {
            return a.codecs < b.codecs;
        }},
        {SortAttribute::RESOLUTION, [](const IFrame& a, const IFrame& b) {
            return a.resolution_height < b.resolution_height;
        }},
        {SortAttribute::VIDEO_RANGE, [](const IFrame& a, const IFrame& b) {
            return a.video_range < b.video_range;
        }},
    };

public:
    // provide access to the comparisons map
    const std::unordered_map<SortAttribute, ComparisonFunc>& getComparisons() const {
        return comparisons_;
    }
};

#endif // SIMPLE_HLS_CLIENT_IFRAME_PARSER_H