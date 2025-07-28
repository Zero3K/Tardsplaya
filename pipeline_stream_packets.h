#ifndef TARDSPLAYA_STREAM_PACKETS_H
#define TARDSPLAYA_STREAM_PACKETS_H

#include "pipeline/pipeline_packet.h"
#include <vector>
#include <string>
#include <chrono>
#include <memory>

namespace Tardsplaya {

/**
 * @brief Base packet type for all streaming data
 */
class StreamPacket : public lexus2k::pipeline::IPacket {
public:
    StreamPacket() : m_timestamp(std::chrono::steady_clock::now()) {}
    virtual ~StreamPacket() = default;

    std::chrono::steady_clock::time_point getTimestamp() const { return m_timestamp; }
    void setTimestamp(std::chrono::steady_clock::time_point timestamp) { m_timestamp = timestamp; }

private:
    std::chrono::steady_clock::time_point m_timestamp;
};

/**
 * @brief HLS Segment data packet
 */
class HLSSegmentPacket : public StreamPacket {
public:
    explicit HLSSegmentPacket(const std::vector<uint8_t>& data, const std::string& url = "")
        : m_data(data), m_url(url), m_duration(0.0) {}

    explicit HLSSegmentPacket(std::vector<uint8_t>&& data, const std::string& url = "")
        : m_data(std::move(data)), m_url(url), m_duration(0.0) {}

    const std::vector<uint8_t>& getData() const { return m_data; }
    const std::string& getUrl() const { return m_url; }
    
    double getDuration() const { return m_duration; }
    void setDuration(double duration) { m_duration = duration; }

    size_t getSize() const { return m_data.size(); }

private:
    std::vector<uint8_t> m_data;
    std::string m_url;
    double m_duration;
};

/**
 * @brief Transport Stream packet data
 */
class TSPacket : public StreamPacket {
public:
    static constexpr size_t TS_PACKET_SIZE = 188;

    explicit TSPacket(const uint8_t* data) {
        std::copy(data, data + TS_PACKET_SIZE, m_data);
    }

    explicit TSPacket(const std::vector<uint8_t>& data) {
        if (data.size() >= TS_PACKET_SIZE) {
            std::copy(data.begin(), data.begin() + TS_PACKET_SIZE, m_data);
        }
    }

    const uint8_t* getData() const { return m_data; }
    size_t getSize() const { return TS_PACKET_SIZE; }

    // Transport stream packet analysis
    uint8_t getSyncByte() const { return m_data[0]; }
    bool isValidPacket() const { return getSyncByte() == 0x47; }
    uint16_t getPID() const { return ((m_data[1] & 0x1F) << 8) | m_data[2]; }
    bool hasPayload() const { return (m_data[3] & 0x10) != 0; }
    bool hasAdaptationField() const { return (m_data[3] & 0x20) != 0; }

    uint32_t getFrameNumber() const { return m_frameNumber; }
    void setFrameNumber(uint32_t frameNumber) { m_frameNumber = frameNumber; }

private:
    uint8_t m_data[TS_PACKET_SIZE];
    uint32_t m_frameNumber = 0;
};

/**
 * @brief Playlist metadata packet
 */
class PlaylistPacket : public StreamPacket {
public:
    struct QualityInfo {
        std::string name;
        std::string url;
        int bandwidth;
        std::string resolution;
        
        bool operator==(const QualityInfo& other) const {
            return name == other.name && url == other.url && 
                   bandwidth == other.bandwidth && resolution == other.resolution;
        }
    };

    explicit PlaylistPacket(const std::string& channel, const std::vector<QualityInfo>& qualities)
        : m_channel(channel), m_qualities(qualities) {}

    const std::string& getChannel() const { return m_channel; }
    const std::vector<QualityInfo>& getQualities() const { return m_qualities; }

private:
    std::string m_channel;
    std::vector<QualityInfo> m_qualities;
};

/**
 * @brief Control/Command packet for pipeline control
 */
class ControlPacket : public StreamPacket {
public:
    enum class Command {
        START,
        STOP,
        PAUSE,
        RESUME,
        SEEK,
        QUALITY_CHANGE,
        ERROR
    };

    explicit ControlPacket(Command cmd, const std::string& data = "")
        : m_command(cmd), m_data(data) {}

    Command getCommand() const { return m_command; }
    const std::string& getData() const { return m_data; }

private:
    Command m_command;
    std::string m_data;
};

/**
 * @brief Statistics packet for monitoring
 */
class StatsPacket : public StreamPacket {
public:
    struct Stats {
        size_t packetsProcessed = 0;
        size_t bytesProcessed = 0;
        size_t droppedFrames = 0;
        double currentFPS = 0.0;
        double bufferLevel = 0.0; // 0.0 to 1.0
        std::chrono::milliseconds latency{0};
    };

    explicit StatsPacket(const Stats& stats) : m_stats(stats) {}

    const Stats& getStats() const { return m_stats; }
    void updateStats(const Stats& stats) { m_stats = stats; }

private:
    Stats m_stats;
};

} // namespace Tardsplaya

#endif // TARDSPLAYA_STREAM_PACKETS_H