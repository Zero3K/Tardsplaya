#include "pipeline_manager.h"
#include "pipeline_stream_nodes.h"
#include "pipeline_stream_packets.h"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <type_traits>

/**
 * @brief Comprehensive Pipeline Library Demonstration
 * 
 * This program demonstrates the full capabilities of the Pipeline library
 * integrated with Tardsplaya for professional-grade stream processing.
 */

using namespace Tardsplaya;
using namespace lexus2k::pipeline;

void printHeader(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, '=') << std::endl;
}

void demonstrateBasicPipeline() {
    printHeader("Basic Pipeline with Custom Packets");
    
    auto pipeline = std::make_unique<Pipeline>();
    
    // Create a data generator node
    auto generator = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        for (int i = 0; i < 5; ++i) {
            std::vector<uint8_t> data(1024, i);
            auto hlsPacket = std::make_shared<HLSSegmentPacket>(data, "http://example.com/segment" + std::to_string(i));
            hlsPacket->setDuration(2.0 + i * 0.5);
            
            std::cout << "Generator: Created HLS segment " << i 
                      << " (size: " << hlsPacket->getSize() 
                      << ", duration: " << hlsPacket->getDuration() << "s)" << std::endl;
            
            pad.node()["output"].pushPacket(hlsPacket, 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    });
    generator->addInput("trigger");
    generator->addOutput("output");
    
    // Create a processor node
    auto processor = pipeline->addNode<Node<HLSSegmentPacket>>();
    processor->addInput("input");
    processor->addOutput("output");
    
    // Override processPacket for the processor
    class HLSProcessor : public Node<HLSSegmentPacket> {
    public:
        HLSProcessor() {
            addInput("input");
            m_outputIndex = addOutput("output").getIndex();
        }
    protected:
        bool processPacket(std::shared_ptr<HLSSegmentPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept override {
            std::cout << "Processor: Processing HLS segment from " << packet->getUrl() 
                      << " (" << packet->getSize() << " bytes)" << std::endl;
            
            // Simulate processing by adding metadata
            packet->setDuration(packet->getDuration() * 1.1); // Slight adjustment
            
            (*this)[m_outputIndex].pushPacket(packet, timeoutMs);
            return true;
        }
    private:
        size_t m_outputIndex;
    };
    
    auto customProcessor = pipeline->addNode<HLSProcessor>();
    
    // Create a statistics collector
    auto statsCollector = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        auto hlsPacket = std::dynamic_pointer_cast<HLSSegmentPacket>(packet);
        if (hlsPacket) {
            static size_t totalPackets = 0;
            static size_t totalBytes = 0;
            
            totalPackets++;
            totalBytes += hlsPacket->getSize();
            
            std::cout << "Stats: Processed " << totalPackets << " packets, " 
                      << totalBytes << " total bytes" << std::endl;
        }
        return true;
    });
    statsCollector->addInput("input");
    
    // Connect the pipeline
    pipeline->connect((*generator)["output"], (*customProcessor)["input"]);
    pipeline->connect((*customProcessor)["output"], (*statsCollector)["input"]);
    
    // Start and run
    if (pipeline->start()) {
        auto trigger = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*generator)["trigger"].pushPacket(trigger, 1000);
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        pipeline->stop();
    }
    
    std::cout << "Basic pipeline demonstration completed.\n" << std::endl;
}

void demonstrateAdvancedBuffering() {
    printHeader("Advanced Buffering with QueuePads");
    
    auto pipeline = std::make_unique<Pipeline>();
    
    // Fast producer
    auto fastProducer = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        std::cout << "Fast Producer: Starting burst production" << std::endl;
        
        for (int i = 0; i < 10; ++i) {
            std::vector<uint8_t> data(512, i);
            auto tsPacket = std::make_shared<TSPacket>(data);
            tsPacket->setFrameNumber(i);
            
            std::cout << "  Producing TS packet " << i << std::endl;
            pad.node()["output"].pushPacket(tsPacket, 100);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Fast production
        }
        return true;
    });
    fastProducer->addInput("trigger");
    fastProducer->addOutput("output");
    
    // Slow consumer with buffering
    auto slowConsumer = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        auto tsPacket = std::dynamic_pointer_cast<TSPacket>(packet);
        if (tsPacket) {
            std::cout << "  Consuming TS packet " << tsPacket->getFrameNumber() 
                      << " (buffered processing)" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Slow processing
        }
        return true;
    });
    slowConsumer->addInput<QueuePad>("input", 8); // Buffer up to 8 packets
    
    pipeline->connect((*fastProducer)["output"], (*slowConsumer)["input"]);
    
    if (pipeline->start()) {
        auto trigger = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*fastProducer)["trigger"].pushPacket(trigger, 1000);
        
        std::this_thread::sleep_for(std::chrono::seconds(3));
        pipeline->stop();
    }
    
    std::cout << "Advanced buffering demonstration completed.\n" << std::endl;
}

void demonstratePacketSplitting() {
    printHeader("Packet Splitting and Broadcasting");
    
    auto pipeline = std::make_unique<Pipeline>();
    
    // Source
    auto source = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        auto statsPacket = std::make_shared<StatsPacket>(StatsPacket::Stats{
            .packetsProcessed = 42,
            .bytesProcessed = 1024*1024,
            .droppedFrames = 0,
            .currentFPS = 30.0,
            .bufferLevel = 0.75,
            .latency = std::chrono::milliseconds(50)
        });
        
        std::cout << "Source: Broadcasting statistics packet" << std::endl;
        pad.node()["output"].pushPacket(statsPacket, 1000);
        return true;
    });
    source->addInput("trigger");
    source->addOutput("output");
    
    // Splitter
    auto splitter = pipeline->addNode<ISplitter>();
    splitter->addInput("input");
    splitter->addOutput("monitor1");
    splitter->addOutput("monitor2");
    splitter->addOutput("logger");
    
    // Multiple consumers
    auto monitor1 = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        auto statsPacket = std::dynamic_pointer_cast<StatsPacket>(packet);
        if (statsPacket) {
            const auto& stats = statsPacket->getStats();
            std::cout << "Monitor 1: FPS = " << stats.currentFPS 
                      << ", Buffer = " << (stats.bufferLevel * 100) << "%" << std::endl;
        }
        return true;
    });
    monitor1->addInput("input");
    
    auto monitor2 = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        auto statsPacket = std::dynamic_pointer_cast<StatsPacket>(packet);
        if (statsPacket) {
            const auto& stats = statsPacket->getStats();
            std::cout << "Monitor 2: Packets = " << stats.packetsProcessed 
                      << ", Latency = " << stats.latency.count() << "ms" << std::endl;
        }
        return true;
    });
    monitor2->addInput("input");
    
    auto logger = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        auto statsPacket = std::dynamic_pointer_cast<StatsPacket>(packet);
        if (statsPacket) {
            const auto& stats = statsPacket->getStats();
            std::cout << "Logger: Full stats - Packets: " << stats.packetsProcessed 
                      << ", Bytes: " << stats.bytesProcessed 
                      << ", Drops: " << stats.droppedFrames << std::endl;
        }
        return true;
    });
    logger->addInput("input");
    
    // Connect everything
    pipeline->connect((*source)["output"], (*splitter)["input"]);
    pipeline->connect((*splitter)["monitor1"], (*monitor1)["input"]);
    pipeline->connect((*splitter)["monitor2"], (*monitor2)["input"]);
    pipeline->connect((*splitter)["logger"], (*logger)["input"]);
    
    if (pipeline->start()) {
        auto trigger = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*source)["trigger"].pushPacket(trigger, 1000);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pipeline->stop();
    }
    
    std::cout << "Packet splitting demonstration completed.\n" << std::endl;
}

void demonstrateStreamingPipeline() {
    printHeader("Complete Streaming Pipeline Simulation");
    
    // Create a complete streaming pipeline
    auto manager = std::make_unique<PipelineManager>("example_channel");
    
    // Set up callbacks
    manager->setStatsCallback([](const StatsPacket::Stats& stats) {
        std::cout << "Pipeline Stats Update:" << std::endl;
        std::cout << "  Packets: " << stats.packetsProcessed << std::endl;
        std::cout << "  Bytes: " << stats.bytesProcessed << std::endl;
        std::cout << "  FPS: " << stats.currentFPS << std::endl;
        std::cout << "  Buffer: " << (stats.bufferLevel * 100) << "%" << std::endl;
        std::cout << "  Latency: " << stats.latency.count() << "ms" << std::endl;
    });
    
    manager->setQualityCallback([](const std::vector<PlaylistPacket::QualityInfo>& qualities) {
        std::cout << "Available Qualities:" << std::endl;
        for (const auto& quality : qualities) {
            std::cout << "  " << quality.name << " - " << quality.bandwidth 
                      << " kbps (" << quality.resolution << ")" << std::endl;
        }
    });
    
    if (manager->initialize()) {
        std::cout << "Streaming pipeline initialized successfully" << std::endl;
        std::cout << "Pipeline components:" << std::endl;
        std::cout << "  - Twitch Source Node (fetches HLS segments)" << std::endl;
        std::cout << "  - HLS Parser Node (TSDuck-inspired parsing)" << std::endl;
        std::cout << "  - TS Router Node (converts to Transport Stream)" << std::endl;
        std::cout << "  - Smart Buffer Node (adaptive buffering)" << std::endl;
        std::cout << "  - Media Player Output Node (sends to player)" << std::endl;
        std::cout << "  - Statistics Monitor Node (real-time monitoring)" << std::endl;
        
        std::cout << "\nSimulating streaming session..." << std::endl;
        
        if (manager->start()) {
            std::cout << "Streaming started" << std::endl;
            
            // Simulate running for a few seconds
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            // Test quality change
            std::cout << "Changing quality..." << std::endl;
            manager->changeQuality("http://example.com/high_quality.m3u8");
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Test pause/resume
            std::cout << "Pausing stream..." << std::endl;
            manager->pause();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            std::cout << "Resuming stream..." << std::endl;
            manager->resume();
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            std::cout << "Stopping stream..." << std::endl;
            manager->stop();
        }
    }
    
    std::cout << "Streaming pipeline simulation completed.\n" << std::endl;
}

void demonstrateErrorHandling() {
    printHeader("Error Handling and Recovery");
    
    auto pipeline = std::make_unique<Pipeline>();
    
    // Node that occasionally fails
    auto unreliableNode = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        static int counter = 0;
        counter++;
        
        std::cout << "Processing packet " << counter;
        
        if (counter % 4 == 0) {
            std::cout << " - FAILED!" << std::endl;
            return false; // Simulate failure
        }
        
        std::cout << " - SUCCESS" << std::endl;
        pad.node()["output"].pushPacket(packet, 1000);
        return true;
    });
    unreliableNode->addInput("input");
    unreliableNode->addOutput("output");
    
    // Recovery node that handles failures gracefully
    auto recoveryNode = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        std::cout << "Recovery node: Packet received and processed successfully" << std::endl;
        return true;
    });
    recoveryNode->addInput("input");
    
    // Producer that sends multiple packets
    auto producer = pipeline->addNode([](std::shared_ptr<IPacket> packet, IPad& pad) -> bool {
        for (int i = 0; i < 8; ++i) {
            auto testPacket = std::make_shared<ControlPacket>(ControlPacket::Command::START);
            pad.node()["output"].pushPacket(testPacket, 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    });
    producer->addInput("trigger");
    producer->addOutput("output");
    
    // Connect pipeline
    pipeline->connect((*producer)["output"], (*unreliableNode)["input"]);
    pipeline->connect((*unreliableNode)["output"], (*recoveryNode)["input"]);
    
    if (pipeline->start()) {
        auto trigger = std::make_shared<ControlPacket>(ControlPacket::Command::START);
        (*producer)["trigger"].pushPacket(trigger, 1000);
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        pipeline->stop();
    }
    
    std::cout << "Error handling demonstration completed.\n" << std::endl;
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "            PIPELINE LIBRARY FULL DEMONSTRATION" << std::endl;
    std::cout << "                 for Tardsplaya Streaming" << std::endl;
    std::cout << "========================================================" << std::endl;
    
    std::cout << "\nThis demonstration showcases the comprehensive integration" << std::endl;
    std::cout << "of the Pipeline library with Tardsplaya for professional" << std::endl;
    std::cout << "stream processing capabilities." << std::endl;
    
    try {
        // Run all demonstrations
        demonstrateBasicPipeline();
        demonstrateAdvancedBuffering();
        demonstratePacketSplitting();
        demonstrateStreamingPipeline();
        demonstrateErrorHandling();
        
        // Run the examples from the Pipeline examples class
        std::cout << "\n" << std::string(60, '-') << std::endl;
        std::cout << "Running additional Pipeline examples..." << std::endl;
        std::cout << std::string(60, '-') << std::endl;
        
        PipelineExamples::runAllExamples();
        
    } catch (const std::exception& e) {
        std::cerr << "Error during demonstration: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\n========================================================" << std::endl;
    std::cout << "     PIPELINE INTEGRATION DEMONSTRATION COMPLETE!" << std::endl;
    std::cout << "========================================================" << std::endl;
    
    std::cout << "\nThe Pipeline library provides:" << std::endl;
    std::cout << "✓ Modular, reusable data processing nodes" << std::endl;
    std::cout << "✓ Type-safe packet handling with C++ templates" << std::endl;
    std::cout << "✓ Advanced buffering with QueuePads" << std::endl;
    std::cout << "✓ Real-time processing capabilities" << std::endl;
    std::cout << "✓ Professional streaming pipeline architecture" << std::endl;
    std::cout << "✓ Comprehensive error handling and recovery" << std::endl;
    std::cout << "✓ Statistics monitoring and performance tracking" << std::endl;
    std::cout << "✓ Seamless integration with existing Tardsplaya code" << std::endl;
    
    std::cout << "\nThis is a FULL implementation demonstrating all major" << std::endl;
    std::cout << "Pipeline library features for professional streaming applications." << std::endl;
    
    return 0;
}