/*
 * Real GPAC Core Integration for Tardsplaya
 * Essential GPAC types and structures for actual MPEG-TS decoding
 */

#ifndef _GPAC_CORE_H_
#define _GPAC_CORE_H_

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

// Essential GPAC error codes and types
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

// Stream types from MPEG-TS standard
#define STREAM_TYPE_VIDEO_MPEG2 0x02
#define STREAM_TYPE_AUDIO_MPEG2 0x03
#define STREAM_TYPE_VIDEO_H264  0x1B
#define STREAM_TYPE_AUDIO_AAC   0x0F

// GPAC M2TS Stream Types 
enum GF_M2TSStreamType {
    GF_M2TS_VIDEO_MPEG1     = 0x01,
    GF_M2TS_VIDEO_MPEG2     = 0x02,
    GF_M2TS_AUDIO_MPEG1     = 0x03,
    GF_M2TS_AUDIO_MPEG2     = 0x04,
    GF_M2TS_PRIVATE_SECTION = 0x05,
    GF_M2TS_PRIVATE_DATA    = 0x06,
    GF_M2TS_AUDIO_AAC       = 0x0F,
    GF_M2TS_VIDEO_MPEG4     = 0x10,
    GF_M2TS_VIDEO_H264      = 0x1B,
    GF_M2TS_VIDEO_HEVC      = 0x24,
    GF_M2TS_VIDEO_VVC       = 0x33,
    GF_M2TS_AUDIO_AC3       = 0x81,
    GF_M2TS_AUDIO_EC3       = 0x87
};

// GPAC essential structures for MPEG-TS
struct GF_M2TS_ES {
    u16 pid;
    GF_M2TSStreamType stream_type;
    u8 *buffer;
    u32 buffer_size;
    u32 buffer_len;
    bool is_video;
    bool is_audio;
    
    // Continuity counter for this PID
    u8 cc;
    
    // PES packet assembly
    u8 *pes_data;
    u32 pes_len;
    u32 pes_expected_len;
    bool pes_start_found;
};

struct GF_M2TS_Program {
    u16 pmt_pid;
    u16 pcr_pid;
    u16 number;
    std::vector<GF_M2TS_ES*> streams;
};

struct GF_M2TS_Demuxer {
    bool pat_found;
    bool pmt_found;
    
    // Programs in this TS
    std::vector<GF_M2TS_Program*> programs;
    
    // All Elementary Streams by PID
    std::map<u16, GF_M2TS_ES*> ESs;
    
    // Callback for elementary stream data
    std::function<void(GF_M2TS_ES*, const u8*, u32)> on_event;
};

// H.264 NAL unit types
#define H264_NAL_SLICE          1
#define H264_NAL_DPA            2
#define H264_NAL_DPB            3
#define H264_NAL_DPC            4
#define H264_NAL_IDR_SLICE      5
#define H264_NAL_SEI            6
#define H264_NAL_SPS            7
#define H264_NAL_PPS            8
#define H264_NAL_AUD            9
#define H264_NAL_END_SEQUENCE   10
#define H264_NAL_END_STREAM     11
#define H264_NAL_FILLER_DATA    12

// H.264 decoder structures
struct H264_SPS {
    u32 profile_idc;
    u32 level_idc;
    u32 seq_parameter_set_id;
    u32 log2_max_frame_num_minus4;
    u32 pic_order_cnt_type;
    u32 log2_max_pic_order_cnt_lsb_minus4;
    u32 max_num_ref_frames;
    bool gaps_in_frame_num_value_allowed_flag;
    u32 pic_width_in_mbs_minus1;
    u32 pic_height_in_map_units_minus1;
    bool frame_mbs_only_flag;
    bool mb_adaptive_frame_field_flag;
    bool direct_8x8_inference_flag;
    bool frame_cropping_flag;
    u32 frame_crop_left_offset;
    u32 frame_crop_right_offset;
    u32 frame_crop_top_offset;
    u32 frame_crop_bottom_offset;
    
    // Derived values
    u32 width;
    u32 height;
    bool valid;
};

struct H264_PPS {
    u32 pic_parameter_set_id;
    u32 seq_parameter_set_id;
    bool entropy_coding_mode_flag;
    bool bottom_field_pic_order_in_frame_present_flag;
    u32 num_slice_groups_minus1;
    u32 num_ref_idx_l0_default_active_minus1;
    u32 num_ref_idx_l1_default_active_minus1;
    bool weighted_pred_flag;
    u32 weighted_bipred_idc;
    s32 pic_init_qp_minus26;
    s32 pic_init_qs_minus26;
    s32 chroma_qp_index_offset;
    bool deblocking_filter_control_present_flag;
    bool constrained_intra_pred_flag;
    bool redundant_pic_cnt_present_flag;
    bool valid;
};

struct H264_DecodeContext {
    H264_SPS sps;
    H264_PPS pps;
    u32 frame_num;
    bool has_sps;
    bool has_pps;
    
    // Current decoded frame
    std::vector<u8> yuv_frame;
    u32 width;
    u32 height;
    u64 pts;
};

// AAC decoder structures
struct AAC_AudioSpecificConfig {
    u32 object_type;
    u32 sampling_frequency_index;
    u32 sampling_frequency;
    u32 channel_configuration;
    bool valid;
};

struct AAC_DecodeContext {
    AAC_AudioSpecificConfig config;
    std::vector<s16> pcm_buffer;
    u32 sample_rate;
    u32 channels;
    bool has_config;
};

// Video frame structure
struct VideoFrame {
    std::vector<u8> rgb_data;
    u32 width;
    u32 height;
    u64 pts;
    bool is_keyframe;
};

// Audio frame structure  
struct AudioFrame {
    std::vector<s16> pcm_data;
    u32 sample_rate;
    u32 channels;
    u32 samples;
    u64 pts;
};

#endif // _GPAC_CORE_H_