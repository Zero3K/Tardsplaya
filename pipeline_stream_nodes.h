#ifndef TARDSPLAYA_PIPELINE_NODES_H
#define TARDSPLAYA_PIPELINE_NODES_H

// Prevent Windows macros from interfering with our code
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include "pipeline/pipeline.h"
#include "pipeline_stream_packets.h"
#include "tardsplaya_mocks.h"
#include <fstream>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <cstddef>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Tardsplaya {

/**
 * @brief Source node that fetches Twitch streams and produces HLS segments
 */
class TwitchSourceNode : public lexus2k::pipeline::Node<ControlPacket> {
public:
    explicit TwitchSourceNode(const std::string& channel)
        : m_channel(channel), m_isActive(false) {
        addInput("control");
        m_segmentOutputIndex = addOutput("segments").getIndex();
        m_playlistOutputIndex = addOutput("playlist").getIndex();
        m_statsOutputIndex = addOutput("stats").getIndex();
    }

    bool start() noexcept override {
        m_isActive = true;
        m_fetchThread = std::async(std::launch::async, [this]() {
            fetchStreamData();
        });
        return true;
    }

    void stop() noexcept override {
        m_isActive = false;
        if (m_fetchThread.valid()) {
            m_fetchThread.wait();
        }
    }

protected:
    bool processPacket(std::shared_ptr<ControlPacket> packet, lexus2k::pipeline::IPad& inputPad, uint32_t timeoutMs) noexcept override {
        switch (packet->getCommand()) {
            case ControlPacket::Command::START:
                startStreaming();
                break;
            case ControlPacket::Command::STOP:
                stopStreaming();
                break;
            case ControlPacket::Command::QUALITY_CHANGE:
                changeQuality(packet->getData());
                break;
            default:
                break;
        }
        return true;
    }

private:
    std::string m_channel;
    std::atomic<bool> m_isActive;
    std::future<void> m_fetchThread;
    size_t m_segmentOutputIndex;
    size_t m_playlistOutputIndex;
    size_t m_statsOutputIndex;
    std::vector<PlaylistPacket::QualityInfo> m_availableQualities;
    std::string m_currentQuality;

    void fetchStreamData() {
        TwitchAPI api;
        while (m_isActive) {
            try {
                // Fetch available qualities
                auto qualities = api.getStreamQualities(m_channel);
                if (!qualities.empty()) {
                    // Convert TwitchAPI::QualityInfo to PlaylistPacket::QualityInfo
                    std::vector<PlaylistPacket::QualityInfo> playlistQualities;
                    for (const auto& q : qualities) {
                        PlaylistPacket::QualityInfo pq;
                        pq.name = q.name;
                        pq.url = q.url;
                        pq.bandwidth = q.bandwidth;
                        pq.resolution = q.resolution;
                        playlistQualities.push_back(pq);
                    }
                    
                    if (playlistQualities != m_availableQualities) {
                        m_availableQualities = playlistQualities;
                        auto playlistPacket = std::make_shared<PlaylistPacket>(m_channel, playlistQualities);
                        (*this)[m_playlistOutputIndex].pushPacket(playlistPacket, 100);
                    }
                }

                // Fetch segment data
                if (!m_currentQuality.empty()) {
                    auto segmentData = api.fetchSegment(m_currentQuality);
                    if (!segmentData.empty()) {
                        auto segmentPacket = std::make_shared<HLSSegmentPacket>(std::move(segmentData));
                        (*this)[m_segmentOutputIndex].pushPacket(segmentPacket, 100);
                    }
                }

                // Update statistics
                StatsPacket::Stats stats;
                stats.packetsProcessed = m_segmentCount;
                stats.bytesProcessed = m_totalBytes;
                auto statsPacket = std::make_shared<StatsPacket>(stats);
                (*this)[m_statsOutputIndex].pushPacket(statsPacket, 100);

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } catch (...) {
                // Handle errors gracefully
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void startStreaming() {
        if (!m_availableQualities.empty()) {
            m_currentQuality = m_availableQualities[0].url;
        }
    }

    void stopStreaming() {
        m_currentQuality.clear();
    }

    void changeQuality(const std::string& qualityUrl) {
        m_currentQuality = qualityUrl;
    }

    std::atomic<size_t> m_segmentCount{0};
    std::atomic<size_t> m_totalBytes{0};
};

/**
 * @brief Parser node that processes HLS segments and extracts metadata
 */
class HLSParserNode : public lexus2k::pipeline::Node<HLSSegmentPacket> {
public:
    HLSParserNode() {
        addInput("input");
        m_outputIndex = addOutput("output").getIndex();
        m_statsOutputIndex = addOutput("stats").getIndex();
    }

protected:
    bool processPacket(std::shared_ptr<HLSSegmentPacket> packet, lexus2k::pipeline::IPad& inputPad, uint32_t timeoutMs) noexcept override {
        try {
            // Use TSDuck-inspired HLS parsing
            TSDuckHLSWrapper parser;
            auto parsedData = parser.parseSegment(packet->getData());
            
            // Create enhanced segment packet with metadata
            auto enhancedPacket = std::make_shared<HLSSegmentPacket>(parsedData);
            enhancedPacket->setDuration(parser.getSegmentDuration());
            enhancedPacket->setTimestamp(packet->getTimestamp());

            // Forward processed packet
            (*this)[m_outputIndex].pushPacket(enhancedPacket, timeoutMs);

            // Update statistics
            m_segmentsProcessed++;
            m_bytesProcessed += packet->getSize();
            
            StatsPacket::Stats stats;
            stats.packetsProcessed = m_segmentsProcessed;
            stats.bytesProcessed = m_bytesProcessed;
            auto statsPacket = std::make_shared<StatsPacket>(stats);
            (*this)[m_statsOutputIndex].pushPacket(statsPacket, 100);

            return true;
        } catch (...) {
            return false;
        }
    }

private:
    size_t m_outputIndex;
    size_t m_statsOutputIndex;
    std::atomic<size_t> m_segmentsProcessed{0};
    std::atomic<size_t> m_bytesProcessed{0};
};

/**
 * @brief TSDuck Transport Stream Router Node
 */
class TSRouterNode : public lexus2k::pipeline::Node<HLSSegmentPacket> {
public:
    TSRouterNode() {
        addInput("input");
        m_outputIndex = addOutput("output").getIndex();
        m_statsOutputIndex = addOutput("stats").getIndex();
    }

    bool start() noexcept override {
        m_router = std::make_unique<TSDuckTransportRouter>();
        return m_router ? (m_router->initialize()) : false;
    }

    void stop() noexcept override {
        if (m_router) {
            (m_router->shutdown());
        }
    }

protected:
    bool processPacket(std::shared_ptr<HLSSegmentPacket> packet, lexus2k::pipeline::IPad& inputPad, uint32_t timeoutMs) noexcept override {
        if (!m_router) return false;

        try {
            // Convert HLS segment to Transport Stream packets
            auto tsPackets = (m_router->convertToTS(packet->getData()));
            
            uint32_t frameNumber = m_currentFrameNumber;
            for (const auto& tsData : tsPackets) {
                auto tsPacket = std::make_shared<TSPacket>(tsData);
                tsPacket->setFrameNumber(frameNumber++);
                tsPacket->setTimestamp(packet->getTimestamp());
                
                (*this)[m_outputIndex].pushPacket(tsPacket, timeoutMs);
            }
            
            m_currentFrameNumber = frameNumber;
            m_packetsGenerated += tsPackets.size();

            // Update statistics
            StatsPacket::Stats stats;
            stats.packetsProcessed = m_packetsGenerated;
            stats.currentFPS = calculateFPS();
            auto statsPacket = std::make_shared<StatsPacket>(stats);
            (*this)[m_statsOutputIndex].pushPacket(statsPacket, 100);

            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::unique_ptr<TSDuckTransportRouter> m_router;
    size_t m_outputIndex;
    size_t m_statsOutputIndex;
    std::atomic<uint32_t> m_currentFrameNumber{0};
    std::atomic<size_t> m_packetsGenerated{0};
    std::chrono::steady_clock::time_point m_lastFPSCalculation;
    size_t m_lastPacketCount = 0;

    double calculateFPS() {
        auto now = std::chrono::steady_clock::now();
        if (m_lastFPSCalculation == std::chrono::steady_clock::time_point{}) {
            m_lastFPSCalculation = now;
            return 0.0;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFPSCalculation).count();
        if (elapsed >= 1000) { // Calculate FPS every second
            size_t currentPackets = m_packetsGenerated;
            double fps = (currentPackets - m_lastPacketCount) * 1000.0 / elapsed;
            m_lastFPSCalculation = now;
            m_lastPacketCount = currentPackets;
            return fps;
        }
        return 0.0;
    }
};

/**
 * @brief Smart Buffer Node with adaptive buffering
 */
class SmartBufferNode : public lexus2k::pipeline::Node<TSPacket> {
public:
    explicit SmartBufferNode(size_t initialBufferSize = 5000, size_t maxBufferSize = 10000)
        : m_currentBufferSize(initialBufferSize), m_maxBufferSize(maxBufferSize) {
        addInput<lexus2k::pipeline::QueuePad>("input", m_currentBufferSize);
        m_outputIndex = addOutput("output").getIndex();
        m_statsOutputIndex = addOutput("stats").getIndex();
    }

protected:
    bool processPacket(std::shared_ptr<TSPacket> packet, lexus2k::pipeline::IPad& inputPad, uint32_t timeoutMs) noexcept override {
        // Analyze packet for adaptive buffering
        analyzePacket(packet);
        
        // Forward packet
        (*this)[m_outputIndex].pushPacket(packet, timeoutMs);
        
        m_packetsBuffered++;
        
        // Periodic buffer optimization
        if (m_packetsBuffered % 1000 == 0) {
            optimizeBuffer();
        }

        // Update statistics
        if (m_packetsBuffered % 100 == 0) {
            StatsPacket::Stats stats;
            stats.packetsProcessed = m_packetsBuffered;
            stats.bufferLevel = calculateBufferLevel();
            auto statsPacket = std::make_shared<StatsPacket>(stats);
            (*this)[m_statsOutputIndex].pushPacket(statsPacket, 100);
        }

        return true;
    }

private:
    size_t m_currentBufferSize;
    size_t m_maxBufferSize;
    size_t m_outputIndex;
    size_t m_statsOutputIndex;
    std::atomic<size_t> m_packetsBuffered{0};
    std::atomic<size_t> m_droppedPackets{0};

    void analyzePacket(std::shared_ptr<TSPacket> packet) {
        // Analyze for key frames, timing, etc.
        if (packet->isValidPacket()) {
            // Track quality metrics
        }
    }

    void optimizeBuffer() {
        // Implement adaptive buffer sizing based on stream characteristics
        // This is a simplified version
        double bufferLevel = calculateBufferLevel();
        if (bufferLevel > 0.9 && m_currentBufferSize < m_maxBufferSize) {
            m_currentBufferSize = std::min(m_currentBufferSize + 1000, m_maxBufferSize);
        } else if (bufferLevel < 0.3 && m_currentBufferSize > 1000) {
            m_currentBufferSize = std::max(m_currentBufferSize - 500, (size_t)1000);
        }
    }

    double calculateBufferLevel() {
        // Simplified buffer level calculation
        return static_cast<double>(m_packetsBuffered % 1000) / 1000.0;
    }
};

/**
 * @brief Output node that sends data to media player
 */
class MediaPlayerOutputNode : public lexus2k::pipeline::Node<TSPacket> {
public:
    explicit MediaPlayerOutputNode(const std::string& playerCommand = "mpv -")
        : m_playerCommand(playerCommand), m_isPlayerRunning(false) {
        addInput<lexus2k::pipeline::QueuePad>("input", 1000);
        m_statsOutputIndex = addOutput("stats").getIndex();
    }

    ~MediaPlayerOutputNode() {
        stop();
    }

    bool start() noexcept override {
        return startPlayer();
    }

    void stop() noexcept override {
        stopPlayer();
    }

#ifdef _WIN32
    /**
     * @brief Gets the player process handle (Windows only)
     */
    HANDLE getPlayerProcessHandle() const {
        return m_playerProcess;
    }
#endif

protected:
    bool processPacket(std::shared_ptr<TSPacket> packet, lexus2k::pipeline::IPad& inputPad, uint32_t timeoutMs) noexcept override {
        if (!m_isPlayerRunning || !packet->isValidPacket()) {
            return false;
        }

        try {
#ifdef _WIN32
            // Write TS packet to player stdin via Windows pipe
            if (m_stdinWrite != INVALID_HANDLE_VALUE) {
                DWORD bytesWritten = 0;
                BOOL result = WriteFile(m_stdinWrite, packet->getData(), 
                                       static_cast<DWORD>(packet->getSize()), 
                                       &bytesWritten, nullptr);
                
                if (result && bytesWritten == packet->getSize()) {
                    m_packetsSent++;
                    m_bytesSent += packet->getSize();

                    // Update statistics periodically
                    if (m_packetsSent % 100 == 0) {
                        StatsPacket::Stats stats;
                        stats.packetsProcessed = m_packetsSent;
                        stats.bytesProcessed = m_bytesSent;
                        auto statsPacket = std::make_shared<StatsPacket>(stats);
                        (*this)[m_statsOutputIndex].pushPacket(statsPacket, 100);
                    }

                    return true;
                }
            }
#else
            // For non-Windows platforms, fall back to file output
            if (m_playerStdin.is_open()) {
                m_playerStdin.write(reinterpret_cast<const char*>(packet->getData()), packet->getSize());
                m_playerStdin.flush();
                
                m_packetsSent++;
                m_bytesSent += packet->getSize();

                // Update statistics periodically
                if (m_packetsSent % 100 == 0) {
                    StatsPacket::Stats stats;
                    stats.packetsProcessed = m_packetsSent;
                    stats.bytesProcessed = m_bytesSent;
                    auto statsPacket = std::make_shared<StatsPacket>(stats);
                    (*this)[m_statsOutputIndex].pushPacket(statsPacket, 100);
                }

                return true;
            }
#endif
        } catch (...) {
            // Handle player errors
        }
        return false;
    }

private:
    std::string m_playerCommand;
    std::atomic<bool> m_isPlayerRunning;
    std::ofstream m_playerStdin;
    size_t m_statsOutputIndex;
    std::atomic<size_t> m_packetsSent{0};
    std::atomic<size_t> m_bytesSent{0};

#ifdef _WIN32
    HANDLE m_playerProcess = INVALID_HANDLE_VALUE;
    HANDLE m_playerThread = INVALID_HANDLE_VALUE;
    HANDLE m_stdinWrite = INVALID_HANDLE_VALUE;
    HANDLE m_stdinRead = INVALID_HANDLE_VALUE;
#endif

    bool startPlayer() {
#ifdef _WIN32
        try {
            // Create pipe for stdin with larger buffer like transport stream router
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = TRUE;
            sa.lpSecurityDescriptor = nullptr;

            // Use larger buffer size for better streaming performance
            DWORD pipeBufferSize = 65536; // 64KB buffer (similar to transport stream router)
            if (!CreatePipe(&m_stdinRead, &m_stdinWrite, &sa, pipeBufferSize)) {
                DWORD error = GetLastError();
                // Log error if possible
                return false;
            }

            // Make sure the write handle to the pipe is not inherited
            if (!SetHandleInformation(m_stdinWrite, HANDLE_FLAG_INHERIT, 0)) {
                CloseHandle(m_stdinRead);
                CloseHandle(m_stdinWrite);
                return false;
            }

            // Set up process information
            STARTUPINFOW si = {};
            si.cb = sizeof(STARTUPINFOW);
            si.hStdInput = m_stdinRead;
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            si.dwFlags |= STARTF_USESTDHANDLES;

            PROCESS_INFORMATION pi = {};

            // Convert player command to wide string  
            std::wstring widePlayerCommand(m_playerCommand.begin(), m_playerCommand.end());
            
            // Build command line like transport stream router - simpler approach
            std::wstring cmdLine = L"\"" + widePlayerCommand.substr(0, widePlayerCommand.find(' ')) + L"\"";
            if (widePlayerCommand.find(' ') != std::wstring::npos) {
                cmdLine += L" " + widePlayerCommand.substr(widePlayerCommand.find(' ') + 1);
            }

            // Launch process with same flags as transport stream router
            if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE, 
                              CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, 
                              nullptr, nullptr, &si, &pi)) {
                DWORD error = GetLastError();
                CloseHandle(m_stdinRead);
                CloseHandle(m_stdinWrite);
                return false;
            }

            m_playerProcess = pi.hProcess;
            m_playerThread = pi.hThread;
            m_isPlayerRunning = true;

            // Set process priority like transport stream router
            SetPriorityClass(pi.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);

            // Close the read end of the pipe in parent process
            CloseHandle(m_stdinRead);
            m_stdinRead = INVALID_HANDLE_VALUE;

            return true;
        } catch (...) {
            return false;
        }
#else
        // For non-Windows platforms, fall back to file output
        m_playerStdin.open("stream_output.ts", std::ios::binary);
        m_isPlayerRunning = m_playerStdin.is_open();
        return m_isPlayerRunning;
#endif
    }

    void stopPlayer() {
        m_isPlayerRunning = false;
        
#ifdef _WIN32
        // Close stdin pipe to signal player to exit
        if (m_stdinWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(m_stdinWrite);
            m_stdinWrite = INVALID_HANDLE_VALUE;
        }
        if (m_stdinRead != INVALID_HANDLE_VALUE) {
            CloseHandle(m_stdinRead);
            m_stdinRead = INVALID_HANDLE_VALUE;
        }

        // Wait for player process to exit gracefully
        if (m_playerProcess != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(m_playerProcess, 5000); // Wait up to 5 seconds
            CloseHandle(m_playerProcess);
            m_playerProcess = INVALID_HANDLE_VALUE;
        }
        if (m_playerThread != INVALID_HANDLE_VALUE) {
            CloseHandle(m_playerThread);
            m_playerThread = INVALID_HANDLE_VALUE;
        }
#else
        if (m_playerStdin.is_open()) {
            m_playerStdin.close();
        }
#endif
    }
};

/**
 * @brief Statistics Monitor Node that aggregates and logs statistics
 */
class StatsMonitorNode : public lexus2k::pipeline::Node<StatsPacket> {
public:
    StatsMonitorNode() {
        addInput<lexus2k::pipeline::QueuePad>("input", 100);
    }

protected:
    bool processPacket(std::shared_ptr<StatsPacket> packet, lexus2k::pipeline::IPad& inputPad, uint32_t timeoutMs) noexcept override {
        const auto& stats = packet->getStats();
        
        // Log statistics (in a real implementation, this might update a GUI)
        m_totalPackets += stats.packetsProcessed;
        m_totalBytes += stats.bytesProcessed;
        m_currentFPS = stats.currentFPS;
        m_bufferLevel = stats.bufferLevel;
        
        // Periodic logging
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastLogTime >= std::chrono::seconds(5)) {
            logStatistics();
            m_lastLogTime = now;
        }
        
        return true;
    }

private:
    size_t m_totalPackets = 0;
    size_t m_totalBytes = 0;
    double m_currentFPS = 0.0;
    double m_bufferLevel = 0.0;
    std::chrono::steady_clock::time_point m_lastLogTime;

    void logStatistics() {
        // In a real implementation, this would update the GUI or write to a log file
        printf("Pipeline Stats - Packets: %zu, Bytes: %zu, FPS: %.2f, Buffer: %.1f%%\n",
               m_totalPackets, m_totalBytes, m_currentFPS, m_bufferLevel * 100.0);
    }
};

} // namespace Tardsplaya

#endif // TARDSPLAYA_PIPELINE_NODES_H