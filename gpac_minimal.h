/*
 * Real GPAC Integration for Tardsplaya
 * Complete GPAC implementation for actual MPEG-TS playback
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
#include <mmsystem.h>
#include <dsound.h>

#include "gpac_core.h"
#include "real_mpegts_parser.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")

// Forward declarations
class SimpleVideoRenderer;
class SimpleAudioRenderer;

// Actual structure definitions using real GPAC implementation
struct GF_FilterSession {
    std::unique_ptr<RealMpegTsParser> ts_parser;
    std::unique_ptr<SimpleVideoRenderer> video_renderer;
    std::unique_ptr<SimpleAudioRenderer> audio_renderer;
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

// Minimal GPAC Filter Session API using real implementation
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
    
private:
    static bool s_initialized;
};

// Simple audio renderer for Windows DirectSound
class SimpleAudioRenderer {
public:
    SimpleAudioRenderer();
    ~SimpleAudioRenderer();
    
    bool Initialize(HWND hwnd, uint32_t sampleRate = 48000, uint32_t channels = 2);
    void Shutdown();
    
    bool PlayAudioFrame(const AudioFrame& frame);
    bool PlayAudioData(const uint8_t* data, size_t size, uint32_t sampleRate, uint32_t channels);
    void SetVolume(float volume); // 0.0 to 1.0
    
private:
    LPDIRECTSOUND8 m_dsound;
    LPDIRECTSOUNDBUFFER8 m_buffer;
    HWND m_hwnd;
    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bufferSize;
    uint32_t m_writePos;
    bool m_initialized;
    
    bool CreateAudioBuffer();
    void DestroyAudioBuffer();
};

// Simple video renderer for Windows
class SimpleVideoRenderer {
public:
    SimpleVideoRenderer();
    ~SimpleVideoRenderer();
    
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    
    bool RenderFrame(const VideoFrame& frame);
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