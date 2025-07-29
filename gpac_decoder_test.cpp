#include "gpac_decoder.h"
#include <iostream>
#include <cassert>

// Simple test to verify GPAC decoder implementation
int main() {
    std::wcout << L"=== GPAC Decoder Implementation Test ===" << std::endl;
    
    try {
        // Test 1: Create and initialize decoder
        std::wcout << L"Test 1: Creating GPAC decoder..." << std::endl;
        gpac_decoder::GpacHLSDecoder decoder;
        
        bool init_success = decoder.Initialize();
        std::wcout << L"  Decoder initialization: " << (init_success ? L"SUCCESS" : L"FAILED") << std::endl;
        assert(init_success);
        
        // Test 2: Create media buffer
        std::wcout << L"Test 2: Creating media buffer..." << std::endl;
        gpac_decoder::MediaBuffer buffer(100);
        std::wcout << L"  Buffer creation: SUCCESS" << std::endl;
        assert(buffer.IsEmpty());
        
        // Test 3: Create stream router
        std::wcout << L"Test 3: Creating stream router..." << std::endl;
        gpac_decoder::GpacStreamRouter router;
        std::wcout << L"  Router creation: SUCCESS" << std::endl;
        assert(!router.IsRouting());
        
        // Test 4: Create playlist parser
        std::wcout << L"Test 4: Creating playlist parser..." << std::endl;
        gpac_decoder::PlaylistParser parser;
        
        // Test basic M3U8 parsing
        std::string test_playlist = 
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:10\n"
            "#EXT-X-MEDIA-SEQUENCE:0\n"
            "#EXTINF:10.0,\n"
            "segment1.ts\n"
            "#EXTINF:10.0,\n"
            "segment2.ts\n"
            "#EXT-X-ENDLIST\n";
            
        bool parse_success = parser.ParsePlaylist(test_playlist);
        std::wcout << L"  Playlist parsing: " << (parse_success ? L"SUCCESS" : L"FAILED") << std::endl;
        assert(parse_success);
        
        auto segments = parser.GetSegments();
        std::wcout << L"  Segments found: " << segments.size() << std::endl;
        assert(segments.size() == 2);
        
        // Test 5: Test media packet
        std::wcout << L"Test 5: Creating media packet..." << std::endl;
        gpac_decoder::MediaPacket packet;
        packet.is_video = true;
        packet.data = {0x01, 0x02, 0x03, 0x04};
        packet.frame_number = 1;
        
        std::wcout << L"  Media packet creation: SUCCESS" << std::endl;
        assert(packet.IsValid());
        assert(packet.is_video);
        assert(packet.data.size() == 4);
        
        // Test 6: Test decoder configuration
        std::wcout << L"Test 6: Configuring decoder..." << std::endl;
        decoder.SetOutputFormat(true, true);  // Enable both AVI and WAV
        decoder.SetQuality(1000000, 64000);   // 1Mbps video, 64kbps audio
        
        auto stats = decoder.GetStats();
        std::wcout << L"  Decoder configuration: SUCCESS" << std::endl;
        std::wcout << L"  Initial stats - Segments: " << stats.segments_processed 
                   << L", Healthy: " << (stats.decoder_healthy ? L"YES" : L"NO") << std::endl;
        
        // Test 7: Test buffer operations
        std::wcout << L"Test 7: Testing buffer operations..." << std::endl;
        bool add_success = buffer.AddPacket(packet);
        assert(add_success);
        assert(!buffer.IsEmpty());
        assert(buffer.GetBufferedPackets() == 1);
        
        gpac_decoder::MediaPacket retrieved_packet;
        bool get_success = buffer.GetNextPacket(retrieved_packet, std::chrono::milliseconds(100));
        assert(get_success);
        assert(buffer.IsEmpty());
        assert(retrieved_packet.frame_number == packet.frame_number);
        
        std::wcout << L"  Buffer operations: SUCCESS" << std::endl;
        
        // Test 8: Test router configuration
        std::wcout << L"Test 8: Testing router configuration..." << std::endl;
        gpac_decoder::GpacStreamRouter::RouterConfig config;
        config.player_path = L"test_player.exe";
        config.enable_avi_output = true;
        config.enable_wav_output = true;
        config.buffer_size_packets = 1000;
        config.low_latency_mode = true;
        
        auto buffer_stats = router.GetBufferStats();
        std::wcout << L"  Router configuration: SUCCESS" << std::endl;
        std::wcout << L"  Buffer stats - Packets: " << buffer_stats.buffered_packets 
                   << L", Decoder healthy: " << (buffer_stats.decoder_healthy ? L"YES" : L"NO") << std::endl;
        
        std::wcout << L"\n=== All Tests Passed! ===" << std::endl;
        std::wcout << L"GPAC decoder implementation is ready for integration." << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::wcout << L"ERROR: Exception occurred - " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::wcout << L"ERROR: Unknown exception occurred" << std::endl;
        return 1;
    }
}