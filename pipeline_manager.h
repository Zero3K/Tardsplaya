#ifndef TARDSPLAYA_PIPELINE_MANAGER_H
#define TARDSPLAYA_PIPELINE_MANAGER_H

#include "pipeline/pipeline.h"
#include "pipeline_stream_nodes.h"
#include "pipeline_stream_packets.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace Tardsplaya {

/**
 * @brief Main Pipeline Manager for Tardsplaya streaming
 * 
 * This class provides a full implementation of the Pipeline library for streaming
 * video content. It demonstrates all major Pipeline features including:
 * - Modular node-based processing
 * - Type-safe packet handling  
 * - Advanced buffering and flow control
 * - Real-time statistics monitoring
 * - Professional Transport Stream processing
 */
class PipelineManager {
public:
    using StatsCallback = std::function<void(const StatsPacket::Stats&)>;
    using QualityCallback = std::function<void(const std::vector<PlaylistPacket::QualityInfo>&)>;

    /**
     * @brief Constructs a Pipeline Manager for the specified channel
     */
    explicit PipelineManager(const std::string& channel);
    
    /**
     * @brief Destructor
     */
    ~PipelineManager();

    /**
     * @brief Initializes the complete processing pipeline
     * @return true if initialization succeeded
     */
    bool initialize();

    /**
     * @brief Starts the streaming pipeline
     * @return true if start succeeded
     */
    bool start();

    /**
     * @brief Stops the streaming pipeline
     */
    void stop();

    /**
     * @brief Changes the stream quality
     * @param qualityUrl The URL of the desired quality stream
     */
    void changeQuality(const std::string& qualityUrl);

    /**
     * @brief Pauses the stream
     */
    void pause();

    /**
     * @brief Resumes the stream
     */
    void resume();

    /**
     * @brief Sets callback for statistics updates
     */
    void setStatsCallback(const StatsCallback& callback) { m_statsCallback = callback; }

    /**
     * @brief Sets callback for quality list updates
     */
    void setQualityCallback(const QualityCallback& callback) { m_qualityCallback = callback; }

    /**
     * @brief Gets current pipeline statistics
     */
    StatsPacket::Stats getCurrentStats() const { return m_currentStats; }

    /**
     * @brief Gets available stream qualities
     */
    const std::vector<PlaylistPacket::QualityInfo>& getAvailableQualities() const { return m_availableQualities; }

    /**
     * @brief Checks if pipeline is currently running
     */
    bool isRunning() const { return m_isRunning; }

    /**
     * @brief Creates a basic text processing pipeline (demonstration)
     */
    static std::unique_ptr<PipelineManager> createTextProcessingExample();

    /**
     * @brief Creates a file processing pipeline (demonstration)
     */
    static std::unique_ptr<PipelineManager> createFileProcessingExample(const std::string& inputFile);

private:
    std::string m_channel;
    std::unique_ptr<lexus2k::pipeline::Pipeline> m_pipeline;
    
    // Pipeline nodes
    TwitchSourceNode* m_sourceNode = nullptr;
    HLSParserNode* m_parserNode = nullptr;
    TSRouterNode* m_routerNode = nullptr;
    SmartBufferNode* m_bufferNode = nullptr;
    MediaPlayerOutputNode* m_outputNode = nullptr;
    StatsMonitorNode* m_statsNode = nullptr;

    // Control and monitoring
    bool m_isRunning = false;
    bool m_isPaused = false;
    StatsPacket::Stats m_currentStats;
    std::vector<PlaylistPacket::QualityInfo> m_availableQualities;
    
    // Callbacks
    StatsCallback m_statsCallback;
    QualityCallback m_qualityCallback;

    /**
     * @brief Sets up the complete streaming pipeline
     */
    void setupStreamingPipeline();

    /**
     * @brief Sets up pipeline connections
     */
    void connectPipeline();

    /**
     * @brief Handles statistics updates from the pipeline
     */
    void handleStatsUpdate(const StatsPacket::Stats& stats);

    /**
     * @brief Handles quality list updates
     */
    void handleQualityUpdate(const std::vector<PlaylistPacket::QualityInfo>& qualities);
};

/**
 * @brief Factory class for creating different types of pipelines
 */
class PipelineFactory {
public:
    /**
     * @brief Creates a complete streaming pipeline for Twitch
     */
    static std::unique_ptr<PipelineManager> createStreamingPipeline(const std::string& channel);

    /**
     * @brief Creates a file processing pipeline
     */
    static std::unique_ptr<lexus2k::pipeline::Pipeline> createFileProcessingPipeline(
        const std::string& inputFile, const std::string& outputFile);

    /**
     * @brief Creates a data transformation pipeline
     */
    static std::unique_ptr<lexus2k::pipeline::Pipeline> createTransformationPipeline();

    /**
     * @brief Creates a monitoring pipeline for statistics
     */
    static std::unique_ptr<lexus2k::pipeline::Pipeline> createMonitoringPipeline();
};

/**
 * @brief Utility class for Pipeline examples and demonstrations
 */
class PipelineExamples {
public:
    /**
     * @brief Demonstrates basic Pipeline usage with lambda nodes
     */
    static void demonstrateLambdaNodes();

    /**
     * @brief Demonstrates advanced buffering with QueuePads
     */
    static void demonstrateAdvancedBuffering();

    /**
     * @brief Demonstrates packet splitting and merging
     */
    static void demonstratePacketSplitting();

    /**
     * @brief Demonstrates type-safe packet processing
     */
    static void demonstrateTypeSafeProcessing();

    /**
     * @brief Demonstrates real-time data processing
     */
    static void demonstrateRealTimeProcessing();

    /**
     * @brief Demonstrates error handling and recovery
     */
    static void demonstrateErrorHandling();

    /**
     * @brief Runs all demonstration examples
     */
    static void runAllExamples();
};

} // namespace Tardsplaya

#endif // TARDSPLAYA_PIPELINE_MANAGER_H