#pragma once
#include <stdint.h>
extern "C" {
#include <libavutil/mem.h>           
#include <libavutil/samplefmt.h>     
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>          
#include <libavcodec/avcodec.h>      
}
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t* buf;
    int      size;  // 样本数
    int      cap;   // 容量
} s16_fifo_t;

typedef struct {
    int initialized;

    AVCodec*        codec;
    AVCodecContext* c;

    // 采样率：sr_in : 8000；sr_out :编码采样率（8k 或 16k）
    int             sr_in;          // 8000（G.711）
    int             sr_out;         // 8000/16000
    int             channels;       // 1
    int64_t         ch_layout;      // MONO
    AVSampleFormat  enc_fmt;        // FLTP
    int             enc_frame_size; // 编码器帧长（1024）
    uint32_t        frame_ms_step;  // 每帧时间步长（ms）：1024/sr_out

    // extradata (ASC)
    uint8_t*        asc;
    int             asc_size;
    int             seq_hdr_sent;

    // PTS（ms）
    int             pts_inited;
    uint32_t        next_pts_ms;    

    s16_fifo_t      s16fifo;
} aac_enc_ctx_t;


int16_t mulaw_decode(uint8_t u);
int16_t alaw_decode(uint8_t a);
void s16_fifo_init(s16_fifo_t* F, int init_cap);
void s16_fifo_free(s16_fifo_t* F);
int s16_fifo_ensure(s16_fifo_t* F, int need_total);
int s16_fifo_write(s16_fifo_t* F, const int16_t* in, int n);
int s16_fifo_read(s16_fifo_t* F, int16_t* out, int n);
int name_to_audio_codec_id(const char* name);
void upsample2x_linear_s16(const int16_t* in, int nin, int16_t* out /*size:2*nin*/);
int flv_push_aac_packet(struct rtmp_context_t* context,const uint8_t* payload, size_t payload_bytes, uint8_t aac_packet_type, uint32_t pts_ms);
int aac_open_if_needed(int ch, struct rtmp_context_t* context);
void aac_close_if_open(int ch);
int send_aac_sequence_header_if_needed(aac_enc_ctx_t* A, struct rtmp_context_t* context, uint32_t pts_ms);
void s16_to_flt(float* dst, const int16_t* src, int n);
int encode_and_push_from_s16(aac_enc_ctx_t* A, struct rtmp_context_t* context);
int transcode_g711_and_push(void* param, const unsigned char *buffer, unsigned int buffer_size, uint32_t pts_ms, int is_ulaw /*1=G711U, 0=G711A*/);
void aac_reset_all(void);
#ifdef __cplusplus
}
#endif
