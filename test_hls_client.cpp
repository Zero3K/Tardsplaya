#include <iostream>
#include <string>

// Test compilation of Simple HLS Client headers (excluding Windows-specific fetcher)
#include "simple_hls_client/hls_tag_parser.h"
#include "simple_hls_client/stream_inf_parser.h"
#include "simple_hls_client/media_parser.h"
#include "simple_hls_client/iframe_parser.h"
#include "simple_hls_client/m3u8_parser.h"

int main() {
    std::cout << "Simple HLS Client integration test" << std::endl;
    
    try {
        // Test M3U8 parser creation
        M3U8Parser parser;
        
        // Test a simple HLS playlist
        std::string testPlaylist = 
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=720x480\n"
            "http://example.com/low.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2560000,RESOLUTION=1280x720\n"
            "http://example.com/mid.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=7680000,RESOLUTION=1920x1080\n"
            "http://example.com/high.m3u8\n";
        
        parser.parse(testPlaylist);
        
        const auto& streamParser = parser.getStreamParser();
        std::cout << "Found " << streamParser.variants_.size() << " stream variants" << std::endl;
        
        for (const auto& variant : streamParser.variants_) {
            std::cout << "Stream: " << variant.getQualityName() 
                      << " (" << variant.bandwidth << " bps)" 
                      << " " << variant.getResolutionString() << std::endl;
        }
        
        // Test sorting functionality
        auto stream_accessor = parser.select<ParserType::STREAM>();
        stream_accessor.sort(HLSTagParser::SortAttribute::BANDWIDTH);
        
        std::cout << "After sorting by bandwidth:" << std::endl;
        for (const auto& variant : streamParser.variants_) {
            std::cout << "Stream: " << variant.getQualityName() 
                      << " (" << variant.bandwidth << " bps)" 
                      << " " << variant.getResolutionString() << std::endl;
        }
        
        std::cout << "Test completed successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}