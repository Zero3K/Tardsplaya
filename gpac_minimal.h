/*
 * Minimal GPAC Integration for Tardsplaya
 * Essential GPAC components for MPEG-TS playback
 */

#ifndef _GPAC_MINIMAL_H_
#define _GPAC_MINIMAL_H_

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

// Forward declarations for minimal GPAC types - actual definitions below
class MpegTsParser;
class SimpleVideoRenderer;

// Actual structure definitions
struct GF_FilterSession {
    std::unique_ptr<MpegTsParser> ts_parser;
    std::unique_ptr<SimpleVideoRenderer> video_renderer;
    HWND video_window;
    bool has_video_output;
    bool has_audio_output;
    
    GF_FilterSession();
    ~GF_FilterSession();
};

struct GF_Filter {
    GF_FilterSession* session;
    bool is_ts_demux;
    
    GF_Filter(GF_FilterSession* sess, bool ts_demux);
};

struct GF_FilterPid;
struct GF_FilterPacket;

// Essential GPAC error codes
typedef uint32_t GF_Err;
#define GF_OK                    0
#define GF_BAD_PARAM            1
#define GF_OUT_OF_MEM           2
#define GF_IO_ERR               3
#define GF_NOT_SUPPORTED        4
#define GF_CORRUPTED_DATA       5
#define GF_EOS                  6
#define GF_BUFFER_TOO_SMALL     7

// Basic GPAC types
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

// MPEG-TS specific constants
#define MPEG2_TS_PACKET_SIZE    188
#define MPEG2_TS_SYNC_BYTE      0x47

// PID constants
#define PID_PAT                 0x0000
#define PID_CAT                 0x0001
#define PID_PMT_TYPICAL         0x0010
#define PID_VIDEO_TYPICAL       0x0100
#define PID_AUDIO_TYPICAL       0x0101

// Stream types
#define STREAM_TYPE_VIDEO_MPEG2 0x02
#define STREAM_TYPE_AUDIO_MPEG2 0x03
#define STREAM_TYPE_VIDEO_H264  0x1B
#define STREAM_TYPE_AUDIO_AAC   0x0F

// Minimal GPAC Filter Session API
class GpacMinimal {
public:
    static bool Initialize();
    static void Shutdown();
    static bool IsInitialized();
    
    // Create filter session for MPEG-TS processing
    static GF_FilterSession* CreateSession();
    static void DeleteSession(GF_FilterSession* session);
    
    // Create MPEG-TS demux filter
    static GF_Filter* CreateTSDemuxFilter(GF_FilterSession* session);
    
    // Feed MPEG-TS data to filter
    static GF_Err FeedTSData(GF_Filter* filter, const uint8_t* data, size_t size);
    
    // Process pending packets
    static GF_Err ProcessSession(GF_FilterSession* session);
    
    // Get decoded video/audio data
    static bool GetVideoFrame(GF_FilterSession* session, uint8_t** data, size_t* size, uint32_t* width, uint32_t* height);
    static bool GetAudioFrame(GF_FilterSession* session, uint8_t** data, size_t* size, uint32_t* sampleRate, uint32_t* channels);
    
private:
    static bool s_initialized;
};

// Type definitions for callbacks
typedef std::function<void(const uint8_t*, size_t, uint32_t, uint32_t)> VideoCallback;
typedef std::function<void(const uint8_t*, size_t, uint32_t, uint32_t)> AudioCallback;

// MPEG-TS Parser helper class
class MpegTsParser {
public:
    struct TsPacket {
        uint16_t pid;
        bool payload_unit_start;
        bool transport_error;
        bool adaptation_field;
        bool payload;
        uint8_t continuity_counter;
        const uint8_t* payload_data;
        size_t payload_size;
    };
    
    struct StreamInfo {
        uint16_t pid;
        uint8_t stream_type;
        bool is_video;
        bool is_audio;
    };
    
    MpegTsParser();
    ~MpegTsParser();
    
    // Parse TS packets
    bool ParsePacket(const uint8_t* packet, TsPacket& parsed);
    bool ProcessPackets(const uint8_t* data, size_t size);
    
    // PAT/PMT processing
    bool ProcessPAT(const TsPacket& packet);
    bool ProcessPMT(const TsPacket& packet);
    
    // Get stream information
    const std::vector<StreamInfo>& GetStreams() const { return m_streams; }
    bool IsInitialized() const { return m_pat_parsed && m_pmt_parsed; }
    
    // Callbacks for decoded data
    void SetVideoCallback(VideoCallback callback);
    void SetAudioCallback(AudioCallback callback);
    
private:
    bool m_pat_parsed;
    bool m_pmt_parsed;
    uint16_t m_pmt_pid;
    std::vector<StreamInfo> m_streams;
    
    // Continuity counters
    std::map<uint16_t, uint8_t> m_continuity_counters;
    
    // Callbacks
    VideoCallback m_video_callback;
    AudioCallback m_audio_callback;
    
    // Helper functions
    uint16_t GetPID(const uint8_t* packet);
    bool ValidatePacket(const uint8_t* packet);
    void ProcessPES(uint16_t pid, const uint8_t* data, size_t size);
};

// Simple video renderer for Windows
class SimpleVideoRenderer {
public:
    SimpleVideoRenderer();
    ~SimpleVideoRenderer();
    
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    
    bool RenderFrame(const uint8_t* data, size_t size, uint32_t width, uint32_t height);
    void Resize(uint32_t width, uint32_t height);
    
private:
    HWND m_hwnd;
    HDC m_hdc;
    HBITMAP m_bitmap;
    void* m_bitmap_data;
    uint32_t m_width;
    uint32_t m_height;
    bool m_initialized;
    
    void CreateBitmap(uint32_t width, uint32_t height);
    void DestroyBitmap();
};

#endif // _GPAC_MINIMAL_H_