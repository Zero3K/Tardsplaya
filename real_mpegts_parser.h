/*
 * Real MPEG-TS Demuxer Implementation for Tardsplaya
 * Header file for actual MPEG-TS parsing and H.264/AAC decoding
 */

#ifndef _REAL_MPEGTS_PARSER_H_
#define _REAL_MPEGTS_PARSER_H_

#include "gpac_core.h"
#include <functional>

class RealMpegTsParser {
public:
    RealMpegTsParser();
    ~RealMpegTsParser();
    
    bool Initialize();
    void Shutdown();
    
    // Process MPEG-TS packets
    bool ProcessTSPackets(const u8* data, u32 size);
    bool ProcessTSPacket(const u8* packet);
    
    // Set callbacks for decoded data
    void SetVideoCallback(std::function<void(const VideoFrame&)> callback);
    void SetAudioCallback(std::function<void(const AudioFrame&)> callback);
    
    // Get current program info
    bool HasValidStreams() const { return m_demux.pmt_found; }
    u32 GetVideoWidth() const { return m_h264_ctx.width; }
    u32 GetVideoHeight() const { return m_h264_ctx.height; }
    
private:
    GF_M2TS_Demuxer m_demux;
    H264_DecodeContext m_h264_ctx;
    AAC_DecodeContext m_aac_ctx;
    
    std::function<void(const VideoFrame&)> m_video_callback;
    std::function<void(const AudioFrame&)> m_audio_callback;
    
    // TS packet parsing
    bool ValidatePacket(const u8* packet);
    u16 GetPID(const u8* packet);
    bool GetPayloadUnitStart(const u8* packet);
    u8 GetContinuityCounter(const u8* packet);
    u32 GetPayload(const u8* packet, const u8** payload);
    
    // Section parsing  
    bool ProcessPAT(const u8* data, u32 size);
    bool ProcessPMT(const u8* data, u32 size, u16 pmt_pid);
    
    // PES processing
    bool ProcessPES(GF_M2TS_ES* es, const u8* data, u32 size);
    bool ExtractPESPayload(const u8* pes_data, u32 pes_len, u8** payload, u32* payload_len, u64* pts);
    
    // Elementary stream processing
    void ProcessVideoES(GF_M2TS_ES* es, const u8* data, u32 size, u64 pts);
    void ProcessAudioES(GF_M2TS_ES* es, const u8* data, u32 size, u64 pts);
    
    // H.264 NAL processing
    bool ExtractNALUnits(const u8* data, u32 size, std::vector<std::vector<u8>>& nal_units);
    bool ProcessH264NAL(const u8* nal_data, u32 nal_size);
    bool ParseSPS(const u8* data, u32 size);
    bool ParsePPS(const u8* data, u32 size);
    bool DecodeSlice(const u8* data, u32 size, bool is_keyframe);
    
    // AAC processing  
    bool ProcessAACFrame(const u8* data, u32 size);
    bool ParseADTSHeader(const u8* data, u32 size, u32* frame_len, AAC_AudioSpecificConfig* config);
    
    // Utility functions
    u32 ReadBits(const u8* data, u32& bit_pos, u32 num_bits);
    u32 ReadUE(const u8* data, u32& bit_pos);
    s32 ReadSE(const u8* data, u32& bit_pos);
    
    // Frame generation
    void GenerateVideoFrame(bool is_keyframe);
    void GenerateAudioFrame(u32 samples);
    
    // Logging
    void LogMessage(const std::wstring& message);
};

#endif // _REAL_MPEGTS_PARSER_H_