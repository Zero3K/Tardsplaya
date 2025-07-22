#define NOMINMAX
#include "hls_pts_reclock.h"
#include "tlsclient/tlsclient.h"
#include <iostream>
#include <fstream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <signal.h>
#include <sstream>
#include <map>
#include <set>
#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>

// Helper function for wide string to UTF-8 conversion
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, NULL, 0, NULL, NULL);
    if (size == 0) return "";
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, NULL, NULL);
    return result;
}

// HTTP GET implementation using TLSClient
bool HttpGetText(const std::wstring& url, std::string& out) {
    static bool initialized = false;
    if (!initialized) {
        TLSClient::InitializeGlobal();
        initialized = true;
    }
    
    // Try TLSClient for better HTTPS support
    return TLSClientHTTP::HttpGetText(url, out);
}

// Stub for AddLog function (not needed in standalone tool)
void AddLog(const std::wstring& msg) {
    // Convert to string and output to stderr for debugging
    std::string narrow_msg(msg.begin(), msg.end());
    std::cerr << "[LOG] " << narrow_msg << std::endl;
}

#endif

using namespace hls_pts_reclock;

// Global flag for graceful shutdown
static std::atomic<bool> g_running{true};

void SignalHandler(int signal) {
    std::cerr << "\nReceived signal " << signal << ", shutting down gracefully...\n";
    g_running = false;
}

// HTTP downloader for HLS segments using existing Tardsplaya HTTP functionality
class TardsplayaHttpDownloader {
public:
    TardsplayaHttpDownloader() = default;
    ~TardsplayaHttpDownloader() = default;
    
    std::vector<uint8_t> DownloadData(const std::string& url) {
        std::vector<uint8_t> data;
        
#ifdef _WIN32
        // Convert to wide string for existing HTTP function
        std::wstring wide_url(url.begin(), url.end());
        std::string response;
        
        bool success = HttpGetText(wide_url, response);
        
        if (!success) {
            std::cerr << "Failed to download from " << url << "\n";
            return data;
        }
        
        // Convert string response to byte vector
        data.assign(response.begin(), response.end());
        
        if (data.empty()) {
            std::cerr << "Downloaded empty response from " << url << "\n";
        }
        
#else
        std::cerr << "HTTP downloading not supported on this platform\n";
        std::cerr << "Attempted URL: " << url << "\n";
#endif
        
        return data;
    }
    
    bool DownloadToFile(const std::string& url, const std::string& filename) {
        auto data = DownloadData(url);
        if (data.empty()) return false;
        
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to create output file: " << filename << "\n";
            return false;
        }
        
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return file.good();
    }
};

using HttpDownloader = TardsplayaHttpDownloader;

// HLS Playlist Parser
class HLSPlaylistParser {
public:
    struct Segment {
        std::string url;
        double duration;
        int64_t sequence_number;
        bool has_discontinuity;
        
        Segment() : duration(0.0), sequence_number(0), has_discontinuity(false) {}
    };
    
    struct Playlist {
        std::vector<Segment> segments;
        double target_duration;
        int64_t media_sequence;
        bool is_live;
        std::string base_url;
        
        Playlist() : target_duration(0.0), media_sequence(0), is_live(true) {}
    };
    
    static std::string ResolveUrl(const std::string& url, const std::string& base_url) {
        // If url is already absolute, return as-is
        if (url.find("http://") == 0 || url.find("https://") == 0) {
            return url;
        }
        
        // Extract base URL directory
        std::string base_dir = base_url;
        size_t last_slash = base_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            base_dir = base_dir.substr(0, last_slash + 1);
        }
        
        // Handle relative URLs
        if (url[0] == '/') {
            // Absolute path - need to get protocol and host from base_url
            size_t proto_end = base_url.find("://");
            if (proto_end != std::string::npos) {
                size_t host_end = base_url.find('/', proto_end + 3);
                if (host_end != std::string::npos) {
                    return base_url.substr(0, host_end) + url;
                }
            }
            return base_url + url;
        } else {
            // Relative path
            return base_dir + url;
        }
    }
    
    static Playlist ParsePlaylist(const std::string& content, const std::string& base_url) {
        Playlist playlist;
        playlist.base_url = base_url;
        
        std::istringstream stream(content);
        std::string line;
        Segment current_segment;
        bool has_extinf = false;
        
        while (std::getline(stream, line)) {
            // Remove carriage return if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            if (line.empty() || line[0] == '#') {
                if (line.find("#EXTINF:") == 0) {
                    // Parse segment duration
                    size_t colon_pos = line.find(':');
                    if (colon_pos != std::string::npos) {
                        std::string duration_str = line.substr(colon_pos + 1);
                        size_t comma_pos = duration_str.find(',');
                        if (comma_pos != std::string::npos) {
                            duration_str = duration_str.substr(0, comma_pos);
                        }
                        try {
                            current_segment.duration = std::stod(duration_str);
                            has_extinf = true;
                        } catch (const std::exception& e) {
                            std::cerr << "Failed to parse segment duration: " << duration_str << "\n";
                        }
                    }
                } else if (line.find("#EXT-X-DISCONTINUITY") == 0) {
                    current_segment.has_discontinuity = true;
                } else if (line.find("#EXT-X-TARGETDURATION:") == 0) {
                    try {
                        playlist.target_duration = std::stod(line.substr(22));
                    } catch (const std::exception& e) {
                        std::cerr << "Failed to parse target duration\n";
                    }
                } else if (line.find("#EXT-X-MEDIA-SEQUENCE:") == 0) {
                    try {
                        playlist.media_sequence = std::stoll(line.substr(22));
                    } catch (const std::exception& e) {
                        std::cerr << "Failed to parse media sequence\n";
                    }
                } else if (line.find("#EXT-X-PLAYLIST-TYPE:") == 0) {
                    std::string type = line.substr(21);
                    playlist.is_live = (type != "VOD");
                } else if (line.find("#EXT-X-ENDLIST") == 0) {
                    // Presence of ENDLIST indicates this is a VOD stream
                    playlist.is_live = false;
                }
            } else {
                // This is a segment URL
                if (has_extinf) {
                    current_segment.url = ResolveUrl(line, base_url);
                    current_segment.sequence_number = playlist.media_sequence + playlist.segments.size();
                    playlist.segments.push_back(current_segment);
                    
                    // Reset for next segment
                    current_segment = Segment();
                    has_extinf = false;
                }
            }
        }
        
        return playlist;
    }
};

// MPEG-TS Parser for PTS extraction
class MPEGTSParser {
public:
    static const int TS_PACKET_SIZE = 188;
    static const int SYNC_BYTE = 0x47;
    
    struct TSPacket {
        uint16_t pid;
        bool payload_unit_start;
        bool has_payload;
        bool has_adaptation;
        uint8_t continuity_counter;
        std::vector<uint8_t> payload;
        
        TSPacket() : pid(0), payload_unit_start(false), has_payload(false), 
                    has_adaptation(false), continuity_counter(0) {}
    };
    
    struct PESPacket {
        uint16_t stream_id;
        int64_t pts;
        int64_t dts;
        std::vector<uint8_t> data;
        
        PESPacket() : stream_id(0), pts(-1), dts(-1) {}
    };
    
    static std::vector<TSPacket> ParseTSData(const std::vector<uint8_t>& data) {
        std::vector<TSPacket> packets;
        
        for (size_t i = 0; i + TS_PACKET_SIZE <= data.size(); i += TS_PACKET_SIZE) {
            if (data[i] != SYNC_BYTE) {
                // Try to find next sync byte
                bool found = false;
                for (size_t j = i + 1; j < data.size(); j++) {
                    if (data[j] == SYNC_BYTE) {
                        i = j - TS_PACKET_SIZE; // Will be incremented by loop
                        found = true;
                        break;
                    }
                }
                if (!found) break;
                continue;
            }
            
            TSPacket packet;
            
            // Parse TS header
            uint16_t header1 = (data[i + 1] << 8) | data[i + 2];
            packet.pid = header1 & 0x1FFF;
            packet.payload_unit_start = (header1 & 0x4000) != 0;
            
            uint8_t flags = data[i + 3];
            packet.has_adaptation = (flags & 0x20) != 0;
            packet.has_payload = (flags & 0x10) != 0;
            packet.continuity_counter = flags & 0x0F;
            
            // Extract payload
            int payload_start = 4;
            if (packet.has_adaptation) {
                int adaptation_length = data[i + 4];
                payload_start = 5 + adaptation_length;
            }
            
            if (packet.has_payload && payload_start < TS_PACKET_SIZE) {
                packet.payload.assign(data.begin() + i + payload_start, data.begin() + i + TS_PACKET_SIZE);
            }
            
            packets.push_back(packet);
        }
        
        return packets;
    }
    
    static PESPacket ParsePES(const std::vector<uint8_t>& data) {
        PESPacket pes;
        
        if (data.size() < 6) return pes;
        
        // Check PES start code
        if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) {
            return pes;
        }
        
        pes.stream_id = data[3];
        
        if (data.size() < 9) return pes;
        
        uint8_t pts_dts_flags = data[7] & 0xC0;
        int header_length = data[8];
        
        int pts_offset = 9;
        
        // Parse PTS
        if (pts_dts_flags & 0x80) {
            if (pts_offset + 5 <= data.size()) {
                pes.pts = ParseTimestamp(data.data() + pts_offset);
                pts_offset += 5;
            }
        }
        
        // Parse DTS
        if (pts_dts_flags & 0x40) {
            if (pts_offset + 5 <= data.size()) {
                pes.dts = ParseTimestamp(data.data() + pts_offset);
            }
        } else {
            pes.dts = pes.pts; // DTS = PTS if not present
        }
        
        // Extract payload
        int payload_start = 9 + header_length;
        if (payload_start < data.size()) {
            pes.data.assign(data.begin() + payload_start, data.end());
        }
        
        return pes;
    }
    
private:
    static int64_t ParseTimestamp(const uint8_t* data) {
        int64_t timestamp = 0;
        timestamp |= ((int64_t)(data[0] & 0x0E)) << 29;
        timestamp |= ((int64_t)(data[1])) << 22;
        timestamp |= ((int64_t)(data[2] & 0xFE)) << 14;
        timestamp |= ((int64_t)(data[3])) << 7;
        timestamp |= ((int64_t)(data[4] & 0xFE)) >> 1;
        return timestamp;
    }
};

// Real HLS processor that downloads and processes actual HLS streams
class HLSProcessor {
private:
    PTSReclocker reclocker_;
    CommandLineInterface::Arguments args_;
    HttpDownloader downloader_;
    
public:
    HLSProcessor(const CommandLineInterface::Arguments& args) 
        : reclocker_(args.reclock_config), args_(args) {}
    
    bool ProcessStream() {
        // Use stderr for informational output when stdout is used for stream data
        std::ostream* info_out = (args_.use_stdout || args_.output_url == "-") ? &std::cerr : &std::cout;
        
        *info_out << "Processing HLS stream: " << args_.input_url << "\n";
        *info_out << "Output: " << args_.output_url << " (format: " << args_.output_format << ")\n";
        
        if (args_.verbose) {
            *info_out << "Configuration:\n";
            *info_out << "  Force monotonicity: " << (args_.reclock_config.force_monotonicity ? "yes" : "no") << "\n";
            *info_out << "  Discontinuity threshold: " << args_.reclock_config.discontinuity_threshold << " μs\n";
            *info_out << "  Delta threshold: " << args_.reclock_config.delta_threshold << " s\n";
        }
        
        // Download and parse HLS playlist
        *info_out << "Downloading playlist from: " << args_.input_url << "\n";
        auto playlist_data = downloader_.DownloadData(args_.input_url);
        if (playlist_data.empty()) {
            std::cerr << "Failed to download HLS playlist from: " << args_.input_url << "\n";
            std::cerr << "This could be due to:\n";
            std::cerr << "  - Network connectivity issues\n";
            std::cerr << "  - Invalid URL or server not responding\n";
            std::cerr << "  - TLS/SSL certificate problems\n";
            std::cerr << "  - Server blocking the request\n";
            return false;
        }
        
        *info_out << "Downloaded " << playlist_data.size() << " bytes of playlist data\n";
        
        std::string playlist_content(playlist_data.begin(), playlist_data.end());
        if (args_.verbose) {
            *info_out << "Playlist content preview:\n";
            *info_out << playlist_content.substr(0, 500) << "\n";
            if (playlist_content.size() > 500) {
                *info_out << "...\n";
            }
        }
        
        auto playlist = HLSPlaylistParser::ParsePlaylist(playlist_content, args_.input_url);
        
        if (playlist.segments.empty()) {
            std::cerr << "No segments found in HLS playlist\n";
            std::cerr << "This could indicate:\n";
            std::cerr << "  - Invalid M3U8 format\n";
            std::cerr << "  - Empty playlist\n";
            std::cerr << "  - Incorrect URL (not pointing to a valid HLS stream)\n";
            return false;
        }
        
        *info_out << "Found " << playlist.segments.size() << " segments in playlist\n";
        if (args_.verbose) {
            *info_out << "Target duration: " << playlist.target_duration << "s\n";
            *info_out << "Media sequence: " << playlist.media_sequence << "\n";
            *info_out << "Is live: " << (playlist.is_live ? "yes" : "no") << "\n";
            
            // Show first few segment URLs for debugging
            *info_out << "First few segment URLs:\n";
            for (size_t i = 0; i < playlist.segments.size() && i < 3; ++i) {
                *info_out << "  " << (i + 1) << ": " << playlist.segments[i].url << "\n";
            }
        }
        
        return ProcessSegments(playlist);
    }
    
private:
    bool ProcessSegments(const HLSPlaylistParser::Playlist& initial_playlist) {
        std::ostream* output_stream = nullptr;
        std::ofstream file_output;
        
        // Set up output stream
        if (args_.use_stdout || args_.output_url == "-") {
            output_stream = &std::cout;
            std::cout.sync_with_stdio(false);
            if (args_.verbose) {
                std::cerr << "Streaming " << args_.output_format << " to stdout...\n";
            }
        } else {
            file_output.open(args_.output_url, std::ios::binary);
            if (!file_output.is_open()) {
                std::cerr << "Failed to create output file: " << args_.output_url << "\n";
                return false;
            }
            output_stream = &file_output;
        }
        
        // For live streams, keep track of processed segments to avoid reprocessing
        std::set<int64_t> processed_sequence_numbers;
        auto current_playlist = initial_playlist;
        int segments_processed = 0;
        std::map<uint16_t, std::vector<uint8_t>> pes_buffers; // PID -> accumulated PES data
        
        // Main processing loop - for live streams this will run indefinitely
        do {
            bool processed_any_segments = false;
            
            for (const auto& segment : current_playlist.segments) {
                if (!g_running) {
                    if (args_.verbose) {
                        std::cerr << "Processing interrupted by user.\n";
                    }
                    return true; // Graceful exit
                }
                
                // For live streams, skip already processed segments
                if (current_playlist.is_live && 
                    processed_sequence_numbers.find(segment.sequence_number) != processed_sequence_numbers.end()) {
                    continue;
                }
                
                if (args_.verbose) {
                    std::cerr << "Processing segment " << (segments_processed + 1) 
                             << " (seq: " << segment.sequence_number << "): " << segment.url << "\n";
                }
                
                // Download segment
                auto segment_data = downloader_.DownloadData(segment.url);
                if (segment_data.empty()) {
                    std::cerr << "Failed to download segment: " << segment.url << "\n";
                    std::cerr << "Skipping this segment and continuing...\n";
                    continue;
                }
                
                if (args_.verbose) {
                    std::cerr << "Downloaded " << segment_data.size() << " bytes for segment " << (segments_processed + 1) << "\n";
                }
                
                // Validate that this looks like MPEG-TS data
                if (segment_data.size() < 188 || segment_data[0] != 0x47) {
                    std::cerr << "Warning: Segment data doesn't appear to be valid MPEG-TS (size: " 
                             << segment_data.size() << ", first byte: 0x" 
                             << std::hex << static_cast<int>(segment_data[0]) << std::dec << ")\n";
                    if (args_.verbose) {
                        std::cerr << "First 16 bytes: ";
                        for (size_t i = 0; i < 16 && i < segment_data.size(); ++i) {
                            std::cerr << std::hex << static_cast<int>(segment_data[i]) << " ";
                        }
                        std::cerr << std::dec << "\n";
                    }
                }
                
                // Parse MPEG-TS packets
                auto ts_packets = MPEGTSParser::ParseTSData(segment_data);
                
                if (args_.verbose) {
                    std::cerr << "Parsed " << ts_packets.size() << " TS packets from segment\n";
                }
                
                // Process packets and apply PTS correction
                std::vector<uint8_t> corrected_data;
                bool segment_has_discontinuity = segment.has_discontinuity;
                
                if (ts_packets.empty()) {
                    // If we can't parse TS packets, pass through original data as fallback
                    std::cerr << "Warning: Could not parse TS packets, using original segment data\n";
                    corrected_data = segment_data;
                } else {
                    for (const auto& ts_packet : ts_packets) {
                        // Accumulate PES data for streams with PTS/DTS
                        if (ts_packet.payload_unit_start && !pes_buffers[ts_packet.pid].empty()) {
                            // Process complete PES packet
                            ProcessPESPacket(pes_buffers[ts_packet.pid], ts_packet.pid, segment_has_discontinuity);
                            pes_buffers[ts_packet.pid].clear();
                        }
                        
                        // Add payload to buffer
                        if (ts_packet.has_payload) {
                            pes_buffers[ts_packet.pid].insert(pes_buffers[ts_packet.pid].end(),
                                                             ts_packet.payload.begin(), ts_packet.payload.end());
                        }
                        
                        // Reconstruct TS packet with corrected data
                        std::vector<uint8_t> ts_packet_data = ReconstructTSPacket(ts_packet);
                        corrected_data.insert(corrected_data.end(), ts_packet_data.begin(), ts_packet_data.end());
                    }
                    
                    // Process any remaining PES buffers
                    for (auto& [pid, buffer] : pes_buffers) {
                        if (!buffer.empty()) {
                            ProcessPESPacket(buffer, pid, segment_has_discontinuity);
                        }
                    }
                }
                
                // Write corrected data to output
                if (!corrected_data.empty()) {
                    output_stream->write(reinterpret_cast<const char*>(corrected_data.data()), corrected_data.size());
                    
                    // Log first packet info for debugging (only for first segment)
                    if (segments_processed == 0 && args_.verbose) {
                        std::cerr << "First corrected TS packet: ";
                        for (size_t i = 0; i < 16 && i < corrected_data.size(); ++i) {
                            std::cerr << std::hex << static_cast<int>(corrected_data[i]) << " ";
                        }
                        std::cerr << std::dec << "\n";
                    }
                } else {
                    std::cerr << "Warning: No corrected data generated for segment " << (segments_processed + 1) << "\n";
                }
                
                if (args_.use_stdout) {
                    output_stream->flush();
                    // Small delay for real-time streaming  
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                
                // Mark this segment as processed for live streams
                if (current_playlist.is_live) {
                    processed_sequence_numbers.insert(segment.sequence_number);
                }
                
                segments_processed++;
                processed_any_segments = true;
                segment_has_discontinuity = false; // Only first segment after discontinuity marker
            }
            
            // For live streams, refresh playlist and continue processing new segments
            if (current_playlist.is_live && g_running) {
                if (!processed_any_segments) {
                    // No new segments found, wait before refreshing
                    if (args_.verbose) {
                        std::cerr << "No new segments found, waiting " 
                                 << static_cast<int>(current_playlist.target_duration / 2.0) << "s before refresh...\n";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        static_cast<int>(current_playlist.target_duration * 500))); // Wait half target duration
                }
                
                // Refresh playlist to get new segments
                if (args_.verbose) {
                    std::cerr << "Refreshing playlist for new segments...\n";
                }
                
                auto playlist_data = downloader_.DownloadData(args_.input_url);
                if (!playlist_data.empty()) {
                    std::string playlist_content(playlist_data.begin(), playlist_data.end());
                    auto new_playlist = HLSPlaylistParser::ParsePlaylist(playlist_content, args_.input_url);
                    if (!new_playlist.segments.empty()) {
                        current_playlist = new_playlist;
                        
                        // Clean up old processed sequence numbers to prevent memory growth
                        if (processed_sequence_numbers.size() > 50) {
                            auto min_seq = current_playlist.media_sequence;
                            auto it = processed_sequence_numbers.begin();
                            while (it != processed_sequence_numbers.end()) {
                                if (*it < min_seq - 10) { // Keep some history
                                    it = processed_sequence_numbers.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                        }
                    } else {
                        std::cerr << "Warning: Refreshed playlist is empty, retrying...\n";
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                } else {
                    std::cerr << "Warning: Failed to refresh playlist, retrying...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
            
        } while (current_playlist.is_live && g_running);
        
        // VOD streams or live streams that have ended
        if (!args_.use_stdout && !args_.output_url.empty() && args_.output_url != "-") {
            file_output.close();
            if (args_.verbose) {
                std::cerr << "Created " << args_.output_format << " output: " << args_.output_url << "\n";
            }
        }
        
        // Print final statistics only for VOD or when exiting live streams
        if (!current_playlist.is_live || !g_running) {
            const auto& stats = reclocker_.GetStats();
            std::cerr << "\nProcessing complete. Statistics:\n";
            std::cerr << "  Segments processed: " << segments_processed << "\n";
            std::cerr << "  Total packets processed: " << stats.total_packets_processed << "\n";
            std::cerr << "  Discontinuities detected: " << stats.discontinuities_detected << "\n";
            std::cerr << "  Timestamp corrections applied: " << stats.timestamp_corrections << "\n";
            std::cerr << "  Total offset applied: " << utils::FormatTimestamp(stats.total_offset_applied) << "\n";
        }
        
        return true;
    }
    
    void ProcessPESPacket(const std::vector<uint8_t>& pes_data, uint16_t pid, bool has_discontinuity) {
        if (pes_data.size() < 6) return;
        
        auto pes = MPEGTSParser::ParsePES(pes_data);
        if (pes.pts == -1 && pes.dts == -1) return; // No timestamps
        
        // Convert to reclocker format
        TimestampInfo timestamp_info;
        timestamp_info.pts = pes.pts;
        timestamp_info.dts = pes.dts;
        timestamp_info.duration = 3600; // Default duration (40ms at 90kHz)
        
        // Apply discontinuity flag if present
        if (has_discontinuity) {
            // Reset reclocker state for this stream
            reclocker_.Reset();
        }
        
        // Process with reclocker
        bool success = reclocker_.ProcessPacket(timestamp_info, pid);
        
        if (args_.debug && (pes.pts != timestamp_info.pts || pes.dts != timestamp_info.dts)) {
            std::cerr << "PID " << pid << " - PTS corrected: " 
                     << utils::FormatTimestamp(pes.pts) << " -> " 
                     << utils::FormatTimestamp(timestamp_info.pts) << "\n";
        }
    }
    
    std::vector<uint8_t> ReconstructTSPacket(const MPEGTSParser::TSPacket& packet) {
        std::vector<uint8_t> ts_data(MPEGTSParser::TS_PACKET_SIZE, 0xFF); // Fill with padding
        
        // TS header (4 bytes)
        ts_data[0] = MPEGTSParser::SYNC_BYTE; // 0x47
        
        // Transport Error Indicator (0), Payload Unit Start Indicator, Transport Priority (0), PID (13 bits)
        ts_data[1] = (packet.payload_unit_start ? 0x40 : 0x00) | ((packet.pid >> 8) & 0x1F);
        ts_data[2] = packet.pid & 0xFF;
        
        // Transport Scrambling Control (00), Adaptation Field Control, Continuity Counter
        uint8_t adaptation_control = 0;
        if (packet.has_adaptation && packet.has_payload) {
            adaptation_control = 0x30; // Both adaptation field and payload present
        } else if (packet.has_adaptation) {
            adaptation_control = 0x20; // Adaptation field only
        } else if (packet.has_payload) {
            adaptation_control = 0x10; // Payload only
        } else {
            adaptation_control = 0x00; // Reserved (should not happen)
        }
        
        ts_data[3] = adaptation_control | (packet.continuity_counter & 0x0F);
        
        int data_start = 4;
        
        // Add adaptation field if needed
        if (packet.has_adaptation) {
            // For simplicity, add minimal adaptation field
            ts_data[4] = 0; // Adaptation field length = 0 (just the length byte)
            data_start = 5;
        }
        
        // Add payload
        if (packet.has_payload && !packet.payload.empty()) {
            size_t payload_size = packet.payload.size();
            size_t max_size = MPEGTSParser::TS_PACKET_SIZE - data_start;
            size_t copy_size = std::min(payload_size, max_size);
            
            std::copy(packet.payload.begin(), packet.payload.begin() + copy_size, 
                     ts_data.begin() + data_start);
                     
            if (args_.verbose && copy_size < payload_size) {
                std::cerr << "Warning: TS packet payload truncated from " << payload_size 
                         << " to " << copy_size << " bytes\n";
            }
        }
        
        return ts_data;
    }
};

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    
    // Parse command line arguments
    CommandLineInterface::Arguments args;
    if (!CommandLineInterface::ParseArguments(argc, argv, args)) {
        CommandLineInterface::PrintUsage(argv[0]);
        return 1;
    }
    
    // Only output header to stderr when using stdout for stream data
    std::ostream* info_out = (args.use_stdout || args.output_url == "-") ? &std::cerr : &std::cout;
    
    *info_out << "HLS PTS Discontinuity Reclock Tool\n";
    *info_out << "==================================\n\n";
    
    try {
        HLSProcessor processor(args);
        if (!processor.ProcessStream()) {
            std::cerr << "Failed to process HLS stream\n";
            return 1;
        }
        
        // Only output completion message to stderr when streaming to stdout
        if (args.use_stdout || args.output_url == "-") {
            std::cerr << "\nStream processing completed successfully.\n";
        } else {
            std::cout << "\nStream processing completed successfully.\n";
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred\n";
        return 1;
    }
    
    return 0;
}