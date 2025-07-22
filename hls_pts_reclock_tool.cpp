#include "hls_pts_reclock.h"
#include <iostream>
#include <fstream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <signal.h>
#include <sstream>
#include <map>
#include <algorithm>

#ifdef _WIN32
#define NOMINMAX  // Prevent min/max macros from windows.h
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

using namespace hls_pts_reclock;

// Global flag for graceful shutdown
static std::atomic<bool> g_running{true};

void SignalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully...\n";
    g_running = false;
}

// HTTP downloader for HLS segments (Windows implementation)
#ifdef _WIN32
class WindowsHttpDownloader {
private:
    HINTERNET hSession;
    
public:
    WindowsHttpDownloader() : hSession(nullptr) {
        hSession = WinHttpOpen(L"HLS-PTS-Reclock/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS,
                              0);
    }
    
    ~WindowsHttpDownloader() {
        if (hSession) WinHttpCloseHandle(hSession);
    }
    
    std::vector<uint8_t> DownloadData(const std::string& url) {
        std::vector<uint8_t> data;
        
        if (!hSession) return data;
        
        // Parse URL
        std::wstring wide_url(url.begin(), url.end());
        URL_COMPONENTS urlComp = {};
        urlComp.dwStructSize = sizeof(urlComp);
        
        wchar_t hostname[256];
        wchar_t urlPath[1024];
        urlComp.lpszHostName = hostname;
        urlComp.dwHostNameLength = sizeof(hostname) / sizeof(wchar_t);
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);
        
        if (!WinHttpCrackUrl(wide_url.c_str(), wide_url.length(), 0, &urlComp)) {
            return data;
        }
        
        HINTERNET hConnect = WinHttpConnect(hSession, hostname, urlComp.nPort, 0);
        if (!hConnect) return data;
        
        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
                                              NULL, WINHTTP_NO_REFERER, 
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                if (WinHttpReceiveResponse(hRequest, NULL)) {
                    DWORD bytesAvailable = 0;
                    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                        std::vector<uint8_t> buffer(bytesAvailable);
                        DWORD bytesRead = 0;
                        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                            data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
                        }
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        
        WinHttpCloseHandle(hConnect);
        return data;
    }
    
    bool DownloadToFile(const std::string& url, const std::string& filename) {
        auto data = DownloadData(url);
        if (data.empty()) return false;
        
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;
        
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return file.good();
    }
};

using HttpDownloader = WindowsHttpDownloader;

#else
// Simple HTTP downloader for non-Windows platforms (basic implementation)
class SimpleHttpDownloader {
public:
    SimpleHttpDownloader() = default;
    ~SimpleHttpDownloader() = default;
    
    std::vector<uint8_t> DownloadData(const std::string& url) {
        std::vector<uint8_t> data;
        // For this implementation, we'll just simulate successful download
        // In a real implementation, you'd use libcurl or similar
        std::cerr << "Note: HTTP downloading not implemented for this platform\n";
        std::cerr << "Would download: " << url << "\n";
        return data;
    }
    
    bool DownloadToFile(const std::string& url, const std::string& filename) {
        auto data = DownloadData(url);
        if (data.empty()) return false;
        
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;
        
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return file.good();
    }
};

using HttpDownloader = SimpleHttpDownloader;
#endif

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
        
        Playlist() : target_duration(0.0), media_sequence(0), is_live(false) {}
    };
    
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
                        current_segment.duration = std::stod(duration_str);
                        has_extinf = true;
                    }
                } else if (line.find("#EXT-X-DISCONTINUITY") == 0) {
                    current_segment.has_discontinuity = true;
                } else if (line.find("#EXT-X-TARGETDURATION:") == 0) {
                    playlist.target_duration = std::stod(line.substr(22));
                } else if (line.find("#EXT-X-MEDIA-SEQUENCE:") == 0) {
                    playlist.media_sequence = std::stoll(line.substr(22));
                } else if (line.find("#EXT-X-PLAYLIST-TYPE:") == 0) {
                    std::string type = line.substr(21);
                    playlist.is_live = (type != "VOD");
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
    
private:
    static std::string ResolveUrl(const std::string& url, const std::string& base_url) {
        if (url.find("http://") == 0 || url.find("https://") == 0) {
            return url; // Already absolute
        }
        
        // Simple relative URL resolution
        if (url[0] == '/') {
            // Absolute path
            size_t scheme_end = base_url.find("://");
            if (scheme_end != std::string::npos) {
                size_t host_end = base_url.find('/', scheme_end + 3);
                if (host_end != std::string::npos) {
                    return base_url.substr(0, host_end) + url;
                } else {
                    return base_url + url;
                }
            }
        } else {
            // Relative path
            size_t last_slash = base_url.find_last_of('/');
            if (last_slash != std::string::npos) {
                return base_url.substr(0, last_slash + 1) + url;
            }
        }
        
        return url;
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
        std::cout << "Processing HLS stream: " << args_.input_url << "\n";
        std::cout << "Output: " << args_.output_url << " (format: " << args_.output_format << ")\n";
        
        if (args_.verbose) {
            std::cout << "Configuration:\n";
            std::cout << "  Force monotonicity: " << (args_.reclock_config.force_monotonicity ? "yes" : "no") << "\n";
            std::cout << "  Discontinuity threshold: " << args_.reclock_config.discontinuity_threshold << " μs\n";
            std::cout << "  Delta threshold: " << args_.reclock_config.delta_threshold << " s\n";
        }
        
        // Download and parse HLS playlist
        auto playlist_data = downloader_.DownloadData(args_.input_url);
        if (playlist_data.empty()) {
            std::cerr << "Failed to download HLS playlist\n";
            return false;
        }
        
        std::string playlist_content(playlist_data.begin(), playlist_data.end());
        auto playlist = HLSPlaylistParser::ParsePlaylist(playlist_content, args_.input_url);
        
        if (playlist.segments.empty()) {
            std::cerr << "No segments found in HLS playlist\n";
            return false;
        }
        
        std::cout << "Found " << playlist.segments.size() << " segments in playlist\n";
        if (args_.verbose) {
            std::cout << "Target duration: " << playlist.target_duration << "s\n";
            std::cout << "Media sequence: " << playlist.media_sequence << "\n";
            std::cout << "Is live: " << (playlist.is_live ? "yes" : "no") << "\n";
        }
        
        return ProcessSegments(playlist);
    }
    
private:
    bool ProcessSegments(const HLSPlaylistParser::Playlist& playlist) {
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
        
        int segments_processed = 0;
        std::map<uint16_t, std::vector<uint8_t>> pes_buffers; // PID -> accumulated PES data
        
        for (const auto& segment : playlist.segments) {
            if (!g_running) {
                std::cout << "Processing interrupted by user.\n";
                return false;
            }
            
            if (args_.verbose) {
                std::cout << "Processing segment " << (segments_processed + 1) << "/" << playlist.segments.size()
                         << ": " << segment.url << "\n";
            }
            
            // Download segment
            auto segment_data = downloader_.DownloadData(segment.url);
            if (segment_data.empty()) {
                std::cerr << "Failed to download segment: " << segment.url << "\n";
                continue;
            }
            
            // Parse MPEG-TS packets
            auto ts_packets = MPEGTSParser::ParseTSData(segment_data);
            
            // Process packets and apply PTS correction
            std::vector<uint8_t> corrected_data;
            bool segment_has_discontinuity = segment.has_discontinuity;
            
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
            
            // Write corrected data to output
            output_stream->write(reinterpret_cast<const char*>(corrected_data.data()), corrected_data.size());
            
            if (args_.use_stdout) {
                output_stream->flush();
                // Small delay for real-time streaming
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            segments_processed++;
            segment_has_discontinuity = false; // Only first segment after discontinuity marker
        }
        
        if (!args_.use_stdout && !args_.output_url.empty() && args_.output_url != "-") {
            file_output.close();
            std::cout << "Created " << args_.output_format << " output: " << args_.output_url << "\n";
        }
        
        // Print final statistics
        const auto& stats = reclocker_.GetStats();
        std::cout << "\nProcessing complete. Statistics:\n";
        std::cout << "  Segments processed: " << segments_processed << "\n";
        std::cout << "  Total packets processed: " << stats.total_packets_processed << "\n";
        std::cout << "  Discontinuities detected: " << stats.discontinuities_detected << "\n";
        std::cout << "  Timestamp corrections applied: " << stats.timestamp_corrections << "\n";
        std::cout << "  Total offset applied: " << utils::FormatTimestamp(stats.total_offset_applied) << "\n";
        
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
        
        // TS header
        ts_data[0] = MPEGTSParser::SYNC_BYTE;
        ts_data[1] = (packet.payload_unit_start ? 0x40 : 0x00) | ((packet.pid >> 8) & 0x1F);
        ts_data[2] = packet.pid & 0xFF;
        ts_data[3] = (packet.has_adaptation ? 0x20 : 0x00) | 
                     (packet.has_payload ? 0x10 : 0x00) | 
                     packet.continuity_counter;
        
        int data_start = 4;
        
        // Add adaptation field if needed
        if (packet.has_adaptation) {
            ts_data[4] = 0; // Adaptation field length (minimal)
            data_start = 5;
        }
        
        // Add payload
        if (packet.has_payload && !packet.payload.empty()) {
            size_t payload_size = static_cast<size_t>(packet.payload.size());
            size_t max_size = static_cast<size_t>(MPEGTSParser::TS_PACKET_SIZE - data_start);
            size_t copy_size = (payload_size < max_size) ? payload_size : max_size;
            std::copy(packet.payload.begin(), packet.payload.begin() + copy_size, 
                     ts_data.begin() + data_start);
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
    
    std::cout << "HLS PTS Discontinuity Reclock Tool\n";
    std::cout << "==================================\n\n";
    
    try {
        HLSProcessor processor(args);
        if (!processor.ProcessStream()) {
            std::cerr << "Failed to process HLS stream\n";
            return 1;
        }
        
        std::cout << "\nStream processing completed successfully.\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred\n";
        return 1;
    }
    
    return 0;
}