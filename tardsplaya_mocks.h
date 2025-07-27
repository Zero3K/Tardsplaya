#ifndef TARDSPLAYA_MOCKS_H
#define TARDSPLAYA_MOCKS_H

#include <vector>
#include <string>
#include <cstdint>
#include <thread>
#include <chrono>

// Mock implementations for Tardsplaya-specific classes
// These allow the Pipeline integration to compile on non-Windows systems

namespace Tardsplaya {

// Mock TwitchAPI class
class TwitchAPI {
public:
    struct QualityInfo {
        std::string name;
        std::string url;
        int bandwidth;
        std::string resolution;
    };

    std::vector<QualityInfo> getStreamQualities(const std::string& channel) {
        // Mock implementation
        (void)channel; // Suppress unused parameter warning
        return {
            {"720p", "http://example.com/720p.m3u8", 3000, "1280x720"},
            {"480p", "http://example.com/480p.m3u8", 1500, "854x480"},
            {"360p", "http://example.com/360p.m3u8", 800, "640x360"}
        };
    }

    std::vector<uint8_t> fetchSegment(const std::string& url) {
        // Mock implementation - simulate segment data
        (void)url; // Suppress unused parameter warning
        std::vector<uint8_t> mockData(188 * 100, 0x47); // Mock TS packets
        
        // Simulate some delay
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        return mockData;
    }
};

// Mock TSDuckHLSWrapper class
class TSDuckHLSWrapper {
public:
    std::vector<uint8_t> parseSegment(const std::vector<uint8_t>& segmentData) {
        // Mock implementation - just return the data unchanged
        return segmentData;
    }

    double getSegmentDuration() const {
        // Mock implementation
        return 2.0; // 2 second segments
    }
};

// Mock TSDuckTransportRouter class
class TSDuckTransportRouter {
public:
    bool initialize() {
        return true; // Mock successful initialization
    }

    void shutdown() {
        // Mock implementation
    }

    std::vector<std::vector<uint8_t>> convertToTS(const std::vector<uint8_t>& hlsData) {
        // Mock implementation - convert HLS data to TS packets
        std::vector<std::vector<uint8_t>> tsPackets;
        
        // Create mock TS packets (188 bytes each)
        size_t numPackets = hlsData.size() / 188;
        if (numPackets == 0) numPackets = 1;
        
        for (size_t i = 0; i < numPackets; ++i) {
            std::vector<uint8_t> packet(188, 0x47); // Start with sync byte
            
            // Add some mock data
            if (hlsData.size() > i * 188) {
                size_t copySize = std::min(size_t(188), hlsData.size() - i * 188);
                std::copy(hlsData.begin() + i * 188, 
                         hlsData.begin() + i * 188 + copySize, 
                         packet.begin());
            }
            
            tsPackets.push_back(packet);
        }
        
        return tsPackets;
    }
};

} // namespace Tardsplaya

#endif // TARDSPLAYA_MOCKS_H