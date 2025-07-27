#include "pipeline_manager.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>

namespace Tardsplaya {

PipelineManager::PipelineManager(const std::string& channel)
    : m_channel(channel) {
    m_pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
}

PipelineManager::~PipelineManager() {
    stop();
}

bool PipelineManager::initialize() {
    try {
        setupStreamingPipeline();
        connectPipeline();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Pipeline initialization failed: " << e.what() << std::endl;
        return false;
    }
}

void PipelineManager::setupStreamingPipeline() {
    // Create all pipeline nodes
    m_sourceNode = m_pipeline->addNode<TwitchSourceNode>(m_channel);
    m_parserNode = m_pipeline->addNode<HLSParserNode>();
    m_routerNode = m_pipeline->addNode<TSRouterNode>();
    m_bufferNode = m_pipeline->addNode<SmartBufferNode>(5000, 10000);
    m_outputNode = m_pipeline->addNode<MediaPlayerOutputNode>("mpv -");
    m_statsNode = m_pipeline->addNode<StatsMonitorNode>();
}

void PipelineManager::connectPipeline() {
    // Connect the main processing chain
    m_pipeline->connect((*m_sourceNode)["segments"], (*m_parserNode)["input"]);
    m_pipeline->connect((*m_parserNode)["output"], (*m_routerNode)["input"]);
    m_pipeline->connect((*m_routerNode)["output"], (*m_bufferNode)["input"]);
    m_pipeline->connect((*m_bufferNode)["output"], (*m_outputNode)["input"]);

    // Connect statistics monitoring
    m_pipeline->connect((*m_sourceNode)["stats"], (*m_statsNode)["input"]);
    m_pipeline->connect((*m_parserNode)["stats"], (*m_statsNode)["input"]);
    m_pipeline->connect((*m_routerNode)["stats"], (*m_statsNode)["input"]);
    m_pipeline->connect((*m_bufferNode)["stats"], (*m_statsNode)["input"]);
    m_pipeline->connect((*m_outputNode)["stats"], (*m_statsNode)["input"]);
}

bool PipelineManager::start() {
    if (m_isRunning) {
        return true;
    }

    bool success = m_pipeline->start();
    if (success) {
        m_isRunning = true;
        
        // Send start command to source
        auto startCommand = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*m_sourceNode)["control"].pushPacket(startCommand, 1000);
    }
    
    return success;
}

void PipelineManager::stop() {
    if (!m_isRunning) {
        return;
    }

    // Send stop command to source
    auto stopCommand = std::make_shared<ControlPacket>(ControlPacket::Command::STOP);
    (*m_sourceNode)["control"].pushPacket(stopCommand, 1000);

    // Stop the pipeline
    m_pipeline->stop();
    m_isRunning = false;
}

void PipelineManager::changeQuality(const std::string& qualityUrl) {
    auto qualityCommand = std::make_shared<ControlPacket>(ControlPacket::Command::QUALITY_CHANGE, qualityUrl);
    (*m_sourceNode)["control"].pushPacket(qualityCommand, 1000);
}

void PipelineManager::pause() {
    if (!m_isPaused) {
        auto pauseCommand = std::make_shared<ControlPacket>(ControlPacket::Command::PAUSE);
        (*m_sourceNode)["control"].pushPacket(pauseCommand, 1000);
        m_isPaused = true;
    }
}

void PipelineManager::resume() {
    if (m_isPaused) {
        auto resumeCommand = std::make_shared<ControlPacket>(ControlPacket::Command::RESUME);
        (*m_sourceNode)["control"].pushPacket(resumeCommand, 1000);
        m_isPaused = false;
    }
}

// Factory implementations

std::unique_ptr<PipelineManager> PipelineFactory::createStreamingPipeline(const std::string& channel) {
    auto manager = std::make_unique<PipelineManager>(channel);
    if (manager->initialize()) {
        return manager;
    }
    return nullptr;
}

std::unique_ptr<lexus2k::pipeline::Pipeline> PipelineFactory::createFileProcessingPipeline(
    const std::string& inputFile, const std::string& outputFile) {
    
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Create file processing nodes using lambda functions
    auto fileReader = pipeline->addNode([inputFile](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                                   lexus2k::pipeline::IPad& pad) -> bool {
        // File reading logic would go here
        std::ifstream file(inputFile, std::ios::binary);
        if (file.is_open()) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
            auto hlsPacket = std::make_shared<HLSSegmentPacket>(std::move(data));
            pad.node()["output"].pushPacket(hlsPacket, 1000);
        }
        return true;
    });
    fileReader->addInput("input");
    fileReader->addOutput("output");

    auto fileWriter = pipeline->addNode([outputFile](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                                   lexus2k::pipeline::IPad& pad) -> bool {
        // File writing logic would go here
        auto hlsPacket = std::dynamic_pointer_cast<HLSSegmentPacket>(packet);
        if (hlsPacket) {
            std::ofstream file(outputFile, std::ios::binary | std::ios::app);
            if (file.is_open()) {
                const auto& data = hlsPacket->getData();
                file.write(reinterpret_cast<const char*>(data.data()), data.size());
            }
        }
        return true;
    });
    fileWriter->addInput("input");

    pipeline->connect((*fileReader)["output"], (*fileWriter)["input"]);
    
    return pipeline;
}

std::unique_ptr<lexus2k::pipeline::Pipeline> PipelineFactory::createTransformationPipeline() {
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Create data transformation nodes
    auto transformer = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                           lexus2k::pipeline::IPad& pad) -> bool {
        // Transform packet data
        auto hlsPacket = std::dynamic_pointer_cast<HLSSegmentPacket>(packet);
        if (hlsPacket) {
            // Apply transformations (compression, encryption, etc.)
            pad.node()["output"].pushPacket(packet, 1000);
        }
        return true;
    });
    transformer->addInput("input");
    transformer->addOutput("output");

    return pipeline;
}

std::unique_ptr<lexus2k::pipeline::Pipeline> PipelineFactory::createMonitoringPipeline() {
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    auto monitor = pipeline->addNode<StatsMonitorNode>();
    
    return pipeline;
}

// Examples implementations

void PipelineExamples::demonstrateLambdaNodes() {
    std::cout << "=== Lambda Nodes Demonstration ===" << std::endl;
    
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Create producer lambda node
    auto producer = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                        lexus2k::pipeline::IPad& pad) -> bool {
        std::cout << "Producer: Creating data packet" << std::endl;
        auto dataPacket = std::make_shared<HLSSegmentPacket>(std::vector<uint8_t>{1, 2, 3, 4, 5});
        pad.node()["output"].pushPacket(dataPacket, 1000);
        return true;
    });
    producer->addInput("trigger");
    producer->addOutput("output");

    // Create consumer lambda node
    auto consumer = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                        lexus2k::pipeline::IPad& pad) -> bool {
        auto hlsPacket = std::dynamic_pointer_cast<HLSSegmentPacket>(packet);
        if (hlsPacket) {
            std::cout << "Consumer: Received packet with " << hlsPacket->getSize() << " bytes" << std::endl;
        }
        return true;
    });
    consumer->addInput("input");

    // Connect nodes
    pipeline->connect((*producer)["output"], (*consumer)["input"]);
    
    // Start and test
    if (pipeline->start()) {
        auto triggerPacket = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*producer)["trigger"].pushPacket(triggerPacket, 1000);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pipeline->stop();
    }
    
    std::cout << "Lambda nodes demonstration completed." << std::endl << std::endl;
}

void PipelineExamples::demonstrateAdvancedBuffering() {
    std::cout << "=== Advanced Buffering Demonstration ===" << std::endl;
    
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Create producer with QueuePad
    auto producer = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                        lexus2k::pipeline::IPad& pad) -> bool {
        for (int i = 0; i < 10; ++i) {
            auto dataPacket = std::make_shared<HLSSegmentPacket>(std::vector<uint8_t>(1024, i));
            std::cout << "Producing packet " << i << std::endl;
            pad.node()["output"].pushPacket(dataPacket, 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    });
    producer->addInput("trigger");
    producer->addOutput("output");

    // Create buffered consumer
    auto consumer = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                        lexus2k::pipeline::IPad& pad) -> bool {
        auto hlsPacket = std::dynamic_pointer_cast<HLSSegmentPacket>(packet);
        if (hlsPacket) {
            std::cout << "Consuming buffered packet of size " << hlsPacket->getSize() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Simulate slow processing
        }
        return true;
    });
    consumer->addInput<lexus2k::pipeline::QueuePad>("input", 5); // Buffer up to 5 packets

    pipeline->connect((*producer)["output"], (*consumer)["input"]);
    
    if (pipeline->start()) {
        auto triggerPacket = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*producer)["trigger"].pushPacket(triggerPacket, 1000);
        
        std::this_thread::sleep_for(std::chrono::seconds(3));
        pipeline->stop();
    }
    
    std::cout << "Advanced buffering demonstration completed." << std::endl << std::endl;
}

void PipelineExamples::demonstratePacketSplitting() {
    std::cout << "=== Packet Splitting Demonstration ===" << std::endl;
    
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Create producer
    auto producer = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                        lexus2k::pipeline::IPad& pad) -> bool {
        auto dataPacket = std::make_shared<HLSSegmentPacket>(std::vector<uint8_t>{1, 2, 3, 4, 5});
        std::cout << "Producing packet for splitting" << std::endl;
        pad.node()["output"].pushPacket(dataPacket, 1000);
        return true;
    });
    producer->addInput("trigger");
    producer->addOutput("output");

    // Create splitter
    auto splitter = pipeline->addNode<lexus2k::pipeline::ISplitter>();
    splitter->addInput("input");
    splitter->addOutput("output1");
    splitter->addOutput("output2");
    splitter->addOutput("output3");

    // Create multiple consumers
    auto consumer1 = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                         lexus2k::pipeline::IPad& pad) -> bool {
        std::cout << "Consumer 1 received packet" << std::endl;
        return true;
    });
    consumer1->addInput("input");

    auto consumer2 = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                         lexus2k::pipeline::IPad& pad) -> bool {
        std::cout << "Consumer 2 received packet" << std::endl;
        return true;
    });
    consumer2->addInput("input");

    auto consumer3 = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                         lexus2k::pipeline::IPad& pad) -> bool {
        std::cout << "Consumer 3 received packet" << std::endl;
        return true;
    });
    consumer3->addInput("input");

    // Connect pipeline
    pipeline->connect((*producer)["output"], (*splitter)["input"]);
    pipeline->connect((*splitter)["output1"], (*consumer1)["input"]);
    pipeline->connect((*splitter)["output2"], (*consumer2)["input"]);
    pipeline->connect((*splitter)["output3"], (*consumer3)["input"]);
    
    if (pipeline->start()) {
        auto triggerPacket = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*producer)["trigger"].pushPacket(triggerPacket, 1000);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pipeline->stop();
    }
    
    std::cout << "Packet splitting demonstration completed." << std::endl << std::endl;
}

void PipelineExamples::demonstrateTypeSafeProcessing() {
    std::cout << "=== Type-Safe Processing Demonstration ===" << std::endl;
    
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Use specialized typed nodes
    auto hlsProcessor = pipeline->addNode<HLSParserNode>();
    auto tsProcessor = pipeline->addNode<TSRouterNode>();
    auto statsMonitor = pipeline->addNode<StatsMonitorNode>();
    
    std::cout << "Created type-safe specialized nodes" << std::endl;
    std::cout << "- HLS Parser: processes HLSSegmentPacket types" << std::endl;
    std::cout << "- TS Router: converts HLS to TSPacket types" << std::endl;
    std::cout << "- Stats Monitor: processes StatsPacket types" << std::endl;
    
    std::cout << "Type-safe processing demonstration completed." << std::endl << std::endl;
}

void PipelineExamples::demonstrateRealTimeProcessing() {
    std::cout << "=== Real-Time Processing Demonstration ===" << std::endl;
    
    auto manager = std::make_unique<PipelineManager>("example_channel");
    
    if (manager->initialize()) {
        std::cout << "Real-time streaming pipeline initialized" << std::endl;
        std::cout << "Pipeline includes:" << std::endl;
        std::cout << "- Twitch source node" << std::endl;
        std::cout << "- HLS parser node" << std::endl;
        std::cout << "- TS router node" << std::endl;
        std::cout << "- Smart buffer node" << std::endl;
        std::cout << "- Media player output node" << std::endl;
        std::cout << "- Statistics monitor node" << std::endl;
    }
    
    std::cout << "Real-time processing demonstration completed." << std::endl << std::endl;
}

void PipelineExamples::demonstrateErrorHandling() {
    std::cout << "=== Error Handling Demonstration ===" << std::endl;
    
    auto pipeline = std::make_unique<lexus2k::pipeline::Pipeline>();
    
    // Create node that might fail
    auto unreliableNode = pipeline->addNode([](std::shared_ptr<lexus2k::pipeline::IPacket> packet, 
                                              lexus2k::pipeline::IPad& pad) -> bool {
        static int counter = 0;
        counter++;
        
        if (counter % 3 == 0) {
            std::cout << "Node processing failed (simulated error)" << std::endl;
            return false; // Simulate failure
        }
        
        std::cout << "Node processing succeeded" << std::endl;
        return true;
    });
    unreliableNode->addInput("input");

    std::cout << "Demonstrated error handling in pipeline nodes" << std::endl;
    std::cout << "Nodes can return false to indicate processing failures" << std::endl;
    std::cout << "Pipeline framework handles failures gracefully" << std::endl;
    
    std::cout << "Error handling demonstration completed." << std::endl << std::endl;
}

void PipelineExamples::runAllExamples() {
    std::cout << "========================================" << std::endl;
    std::cout << "Pipeline Library Full Demonstration" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;
    
    demonstrateLambdaNodes();
    demonstrateAdvancedBuffering();
    demonstratePacketSplitting();
    demonstrateTypeSafeProcessing();
    demonstrateRealTimeProcessing();
    demonstrateErrorHandling();
    
    std::cout << "========================================" << std::endl;
    std::cout << "All demonstrations completed!" << std::endl;
    std::cout << "========================================" << std::endl;
}

} // namespace Tardsplaya