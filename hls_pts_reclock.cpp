#include "hls_pts_reclock.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace hls_pts_reclock {

    // Constants
    static const int64_t AV_NOPTS_VALUE = INT64_MIN;
    static const int64_t AV_TIME_BASE = 1000000; // 1 second in microseconds

    PTSReclocker::PTSReclocker(const ReclockConfig& config) 
        : config_(config) {
        Reset();
    }

    PTSReclocker::~PTSReclocker() = default;

    bool PTSReclocker::ProcessPacket(TimestampInfo& packet_info, int stream_index) {
        // Ensure we have enough stream states
        if (stream_index >= static_cast<int>(stream_states_.size())) {
            stream_states_.resize(stream_index + 1);
        }
        
        StreamState& state = stream_states_[stream_index];
        last_discontinuity_detected_ = false;
        stats_.total_packets_processed++;

        // Skip processing if monotonicity is disabled
        if (!config_.force_monotonicity) {
            return true;
        }

        // Skip if no valid timestamps
        if (!utils::IsValidTimestamp(packet_info.pts) && !utils::IsValidTimestamp(packet_info.dts)) {
            return true;
        }

        // Initialize state on first packet
        if (!state.initialized) {
            state.next_pts = packet_info.pts;
            state.next_dts = packet_info.dts;
            state.last_timestamp = packet_info.dts != AV_NOPTS_VALUE ? packet_info.dts : packet_info.pts;
            state.initialized = true;
            return true;
        }

        // Detect discontinuity
        if (DetectDiscontinuity(packet_info, state)) {
            last_discontinuity_detected_ = true;
            stats_.discontinuities_detected++;
        }

        // Apply monotonicity correction if needed
        if (config_.force_monotonicity) {
            ApplyMonotonicityCorrection(packet_info, state);
        }

        // Update state for next packet
        state.next_pts = packet_info.pts + packet_info.duration;
        state.next_dts = packet_info.dts + packet_info.duration;
        if (utils::IsValidTimestamp(packet_info.dts)) {
            state.last_timestamp = packet_info.dts;
        } else if (utils::IsValidTimestamp(packet_info.pts)) {
            state.last_timestamp = packet_info.pts;
        }

        return true;
    }

    void PTSReclocker::Reset() {
        stream_states_.clear();
        last_discontinuity_detected_ = false;
        stats_ = Stats{};
    }

    bool PTSReclocker::DetectDiscontinuity(const TimestampInfo& packet, StreamState& state) {
        // Check for backward timestamp movement or large forward jumps
        if (utils::IsValidTimestamp(packet.dts) && utils::IsValidTimestamp(state.next_dts)) {
            int64_t dts_delta = CalculateTimeDelta(packet.dts, state.next_dts);
            int64_t threshold = utils::CalculateThreshold(config_.delta_threshold, AV_TIME_BASE);
            
            if (dts_delta < -threshold || dts_delta > threshold) {
                return true;
            }
        }

        if (utils::IsValidTimestamp(packet.pts) && utils::IsValidTimestamp(state.next_pts)) {
            int64_t pts_delta = CalculateTimeDelta(packet.pts, state.next_pts);
            int64_t threshold = utils::CalculateThreshold(config_.delta_threshold, AV_TIME_BASE);
            
            if (pts_delta < -threshold || pts_delta > threshold) {
                return true;
            }
        }

        return false;
    }

    void PTSReclocker::ApplyMonotonicityCorrection(TimestampInfo& packet, StreamState& state) {
        int64_t pts_error = 0;
        int64_t dts_error = 0;
        int64_t threshold = config_.discontinuity_threshold;

        // Calculate errors
        if (utils::IsValidTimestamp(packet.pts) && utils::IsValidTimestamp(state.next_pts)) {
            pts_error = state.next_pts - packet.pts;
        }
        
        if (utils::IsValidTimestamp(packet.dts) && utils::IsValidTimestamp(state.next_dts)) {
            dts_error = state.next_dts - packet.dts;
        }

        // Apply correction if error exceeds threshold
        if (std::abs(dts_error) > threshold || std::abs(pts_error) > threshold) {
            if (utils::IsValidTimestamp(packet.pts)) {
                packet.pts += state.monotonicity_offset;
            }
            
            if (utils::IsValidTimestamp(packet.dts)) {
                packet.dts += state.monotonicity_offset;
            }

            // Update offset for future packets
            if (std::abs(dts_error) > std::abs(pts_error) && dts_error != 0) {
                state.monotonicity_offset += dts_error;
                stats_.total_offset_applied += dts_error;
            } else if (pts_error != 0) {
                state.monotonicity_offset += pts_error;
                stats_.total_offset_applied += pts_error;
            }

            stats_.timestamp_corrections++;
        } else {
            // Apply accumulated offset
            if (utils::IsValidTimestamp(packet.pts)) {
                packet.pts += state.monotonicity_offset;
            }
            
            if (utils::IsValidTimestamp(packet.dts)) {
                packet.dts += state.monotonicity_offset;
            }
        }
    }

    int64_t PTSReclocker::CalculateTimeDelta(int64_t current, int64_t previous) {
        if (!utils::IsValidTimestamp(current) || !utils::IsValidTimestamp(previous)) {
            return 0;
        }
        return current - previous;
    }

    // Utility functions implementation
    namespace utils {
        
        int64_t RescaleTime(int64_t timestamp, int64_t from_timebase, int64_t to_timebase) {
            if (!IsValidTimestamp(timestamp)) {
                return AV_NOPTS_VALUE;
            }
            
            if (from_timebase == to_timebase) {
                return timestamp;
            }
            
            // Simple rescaling - in a real implementation you'd use av_rescale_q
            return timestamp * to_timebase / from_timebase;
        }
        
        bool IsValidTimestamp(int64_t timestamp) {
            return timestamp != AV_NOPTS_VALUE && timestamp >= 0;
        }
        
        std::string FormatTimestamp(int64_t timestamp) {
            if (!IsValidTimestamp(timestamp)) {
                return "N/A";
            }
            
            double seconds = static_cast<double>(timestamp) / AV_TIME_BASE;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6) << seconds << "s";
            return oss.str();
        }
        
        int64_t CalculateThreshold(double threshold_seconds, int64_t timebase) {
            return static_cast<int64_t>(threshold_seconds * timebase);
        }
    }

    // Command Line Interface implementation
    bool CommandLineInterface::ParseArguments(int argc, char* argv[], Arguments& args) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            
            if (arg == "-i" && i + 1 < argc) {
                args.input_url = argv[++i];
            } else if (arg == "-o" && i + 1 < argc) {
                args.output_url = argv[++i];
            } else if (arg == "-f" && i + 1 < argc) {
                args.output_format = argv[++i];
            } else if (arg == "--force-monotonicity") {
                args.reclock_config.force_monotonicity = true;
            } else if (arg == "--no-monotonicity") {
                args.reclock_config.force_monotonicity = false;
            } else if (arg == "--threshold" && i + 1 < argc) {
                args.reclock_config.discontinuity_threshold = std::stoll(argv[++i]);
            } else if (arg == "--delta-threshold" && i + 1 < argc) {
                args.reclock_config.delta_threshold = std::stod(argv[++i]);
            } else if (arg == "-v" || arg == "--verbose") {
                args.verbose = true;
            } else if (arg == "--debug") {
                args.debug = true;
            } else if (arg == "-h" || arg == "--help") {
                return false;
            } else if (arg == "--version") {
                PrintVersion();
                return false;
            } else if (args.input_url.empty()) {
                args.input_url = arg;
            } else if (args.output_url.empty()) {
                args.output_url = arg;
            }
        }
        
        return !args.input_url.empty() && !args.output_url.empty();
    }

    void CommandLineInterface::PrintUsage(const char* program_name) {
        std::cout << "HLS PTS Discontinuity Reclock Tool\n";
        std::cout << "Usage: " << program_name << " [options] input_url output_url\n\n";
        std::cout << "Options:\n";
        std::cout << "  -i URL              Input HLS URL\n";
        std::cout << "  -o URL              Output URL (file or stream)\n";
        std::cout << "  -f FORMAT           Output format (mpegts, flv) [default: mpegts]\n";
        std::cout << "  --force-monotonicity Enable PTS discontinuity correction [default]\n";
        std::cout << "  --no-monotonicity   Disable PTS discontinuity correction\n";
        std::cout << "  --threshold USEC    Discontinuity threshold in microseconds [default: 1000000]\n";
        std::cout << "  --delta-threshold S Delta threshold in seconds [default: 10.0]\n";
        std::cout << "  -v, --verbose       Verbose output\n";
        std::cout << "  --debug             Debug output\n";
        std::cout << "  -h, --help          Show this help\n";
        std::cout << "  --version           Show version\n\n";
        std::cout << "Examples:\n";
        std::cout << "  " << program_name << " http://example.com/playlist.m3u8 output.ts\n";
        std::cout << "  " << program_name << " -f flv http://example.com/playlist.m3u8 rtmp://server/stream\n";
    }

    void CommandLineInterface::PrintVersion() {
        std::cout << "HLS PTS Discontinuity Reclock Tool v1.0\n";
        std::cout << "Based on ffmpeg-hls-pts-discontinuity-reclock by Jason Justman\n";
        std::cout << "Integrated with Tardsplaya\n";
    }

} // namespace hls_pts_reclock