#include <stdint.h>
#include "g711_to_aac.h"
#include <cstring>
#include "doordvr_export.h"
#include "ms_netdvr_common.h"
#include "rtmp-client.h"
#include "rtmp_ms.h" 
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

enum {
    RTMP_CTX_COUNT = (int)(sizeof(((struct rtmp_ctrl_t*)0)->rtmp_ctx) / sizeof(((struct rtmp_ctrl_t*)0)->rtmp_ctx[0]))
};
#if RTMP_CTX_COUNT <= 0
#  undef RTMP_CTX_COUNT
#  define RTMP_CTX_COUNT 16
#endif


static aac_enc_ctx_t g_aac[RTMP_CTX_COUNT];
int g_aac_active = 0;
static inline bool ch_ok(int ch) { return ch >= 0 && ch < RTMP_CTX_COUNT; }
unsigned s_audio_sent = 0;

extern "C" void aac_reset_all(void)
{
    for (int ch = 0; ch < 16; ++ch) {
        aac_close_if_open(ch); 
    }
}

int16_t mulaw_decode(uint8_t u)
{
    u = ~u;
    int sign = (u & 0x80);
    int exponent = (u >> 4) & 0x07;
    int mantissa = (u & 0x0F);
    int sample = ((mantissa << 3) + 0x84) << exponent;
    sample -= 0x84;
    return (int16_t)(sign ? -sample : sample);
}

int16_t alaw_decode(uint8_t a)
{
    a ^= 0x55;
    int sign = a & 0x80;
    int exponent = (a & 0x70) >> 4;
    int mantissa = a & 0x0F;
    int sample = 0;
    if (exponent > 0)
        sample = ((mantissa << 4) + 0x100) << (exponent - 1);
    else
        sample = (mantissa << 4) + 8;
    return (int16_t)(sign ? -sample : sample);
}

void s16_fifo_init(s16_fifo_t* F, int init_cap)
{
    memset(F, 0, sizeof(*F));
    if (init_cap < 1024) init_cap = 1024;
    F->buf = (int16_t*)av_malloc(sizeof(int16_t) * init_cap);
    F->cap = F->buf ? init_cap : 0;
    F->size = 0;
}

void s16_fifo_free(s16_fifo_t* F)
{
    if (F->buf) av_free(F->buf);
    memset(F, 0, sizeof(*F));
}

int s16_fifo_ensure(s16_fifo_t* F, int need_total)
{
    if (need_total <= F->cap) return 0;
    int new_cap = F->cap ? F->cap : 1024;
    while (new_cap < need_total) new_cap <<= 1;
    int16_t* nb = (int16_t*)av_realloc(F->buf, sizeof(int16_t) * new_cap);
    if (!nb) return -1;
    F->buf = nb; F->cap = new_cap;
    return 0;
}

int s16_fifo_write(s16_fifo_t* F, const int16_t* in, int n)
{
    if (n <= 0) return 0;
    if (s16_fifo_ensure(F, F->size + n) != 0) return -1;
    memcpy(F->buf + F->size, in, sizeof(int16_t) * n);
    F->size += n;
    return n;
}

int s16_fifo_read(s16_fifo_t* F, int16_t* out, int n)
{
    if (n <= 0 || F->size < n) return 0;
    memcpy(out, F->buf, sizeof(int16_t) * n);
    memmove(F->buf, F->buf + n, sizeof(int16_t) * (F->size - n));
    F->size -= n;
    return n;
}

int name_to_audio_codec_id(const char* name)
{
    if (!name || !*name) return MS_AUDIO_CODEC_G711U; // 默认 PCMU
#if defined(_WIN32)
    auto eq = [](const char* a, const char* b){ return _stricmp(a,b)==0; };
#else
    auto eq = [](const char* a, const char* b){ return strcasecmp(a,b)==0; };
#endif
    if (eq(name, "G711A") || eq(name, "PCMA") || eq(name, "A-LAW") || eq(name, "ALAW"))
        return MS_AUDIO_CODEC_G711A;
    if (eq(name, "G711U") || eq(name, "PCMU") || eq(name, "U-LAW") || eq(name, "ULAW"))
        return MS_AUDIO_CODEC_G711U;
    if (eq(name, "G726"))
        return MS_AUDIO_CODEC_G726;
    return MS_AUDIO_CODEC_G711U;
}

void upsample2x_linear_s16(const int16_t* in, int nin, int16_t* out)
{

    for (int i = 0; i < nin; ++i) {
        int16_t a = in[i];
        out[2*i] = a;
        int16_t b = (i+1 < nin) ? in[i+1] : in[i];
        out[2*i + 1] = (int16_t)((a + b) / 2);
    }
}

int flv_push_aac_packet(struct rtmp_context_t* context,const uint8_t* payload, size_t payload_bytes, uint8_t aac_packet_type, uint32_t pts_ms)
{
    if (!context || !context->rtmp_client || !context->start_rtmp) return 0;
    if (!ch_ok(context->channel)) return 0;

    uint8_t* out = (uint8_t*)av_malloc(2 + payload_bytes);
    if (!out) return -1;
    out[0] = 0xAE;
    out[1] = aac_packet_type; // 0=序列头, 1=原始帧
    if (payload_bytes > 0 && payload) memcpy(out + 2, payload, payload_bytes);

    int ret = rtmp_client_push_audio(context->rtmp_client, out, 2 + payload_bytes, pts_ms);
    av_free(out);
    return ret;
}


int aac_open_if_needed(int ch, struct rtmp_context_t* context)
{
    if (!ch_ok(ch)) {
        DVR_ERROR("aac_open_if_needed: ch=%d out of range [0..%d]", ch, RTMP_CTX_COUNT - 1);
        return -1;
    }
    aac_enc_ctx_t* A = &g_aac[ch];
    if (A->initialized) return 0;

    memset(A, 0, sizeof(*A));
    A->codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!A->codec) { DVR_ERROR("AAC encoder not found"); return -1; }

    A->c = avcodec_alloc_context3(A->codec);
    if (!A->c) { DVR_ERROR("avcodec_alloc_context3 failed"); return -1; }

    A->sr_in   = 8000; 
    A->sr_out  = (context && context->aac_sr == 16000) ? 16000 : 8000; 
    A->channels  = 1;
    A->ch_layout = AV_CH_LAYOUT_MONO;
    A->enc_fmt   = AV_SAMPLE_FMT_FLTP;

    A->c->sample_rate     = A->sr_out;
    A->c->sample_fmt      = A->enc_fmt;
    A->c->bit_rate        = (A->sr_out == 16000 ? 64000 : 48000);
    A->c->time_base       = (AVRational){1, A->sr_out};
    A->c->channels        = A->channels;
    A->c->channel_layout  = A->ch_layout;
    A->c->flags          |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(A->c, A->codec, NULL) < 0) {
        DVR_ERROR("avcodec_open2(AAC) failed");
        return -1;
    }

    A->enc_frame_size = (A->c->frame_size > 0) ? A->c->frame_size : 1024;
    A->frame_ms_step  = (uint32_t)((1000ULL * A->enc_frame_size) / A->sr_out); // 8k->128ms, 16k->64ms

    // extradata -> ASC
    if (A->c->extradata && A->c->extradata_size > 0) {
        A->asc_size = A->c->extradata_size;
        A->asc = (uint8_t*)av_malloc(A->asc_size);
        if (!A->asc) { DVR_ERROR("av_malloc asc failed"); return -1; }
        memcpy(A->asc, A->c->extradata, A->asc_size);
    } else {
        DVR_ERROR("AAC extradata(ASC) missing");
        return -1;
    }

    A->seq_hdr_sent = 0;
    A->pts_inited   = 0;
    A->next_pts_ms  = 0;

    s16_fifo_init(&A->s16fifo, 4096);

    MSLOG_INFO("AAC opened: mono FLTP, out_sr=%d, bitrate=%d, frame_size=%d, ASC=%d",
           A->sr_out, (int)A->c->bit_rate, A->enc_frame_size, A->asc_size);
    A->initialized  = 1;
    if (g_aac_active < RTMP_CTX_COUNT) g_aac_active++;
    return 0;
}

void aac_close_if_open(int ch)
{
    if (!ch_ok(ch)) return;
    aac_enc_ctx_t* A = &g_aac[ch];

    s16_fifo_free(&A->s16fifo);

    if (A->c) { avcodec_free_context(&A->c); A->c = NULL; }
    if (A->asc) { av_free(A->asc); A->asc = NULL; }

    int was_init = A->initialized;
    memset(A, 0, sizeof(*A));

    if (was_init && g_aac_active > 0) g_aac_active--;
    {
        char list[4 * RTMP_CTX_COUNT + 8];
        int pos = 0;
        list[0] = '\0';
        for (int i = 0; i < RTMP_CTX_COUNT; ++i) {
            if (g_aac[i].initialized) {
                pos += snprintf(list + pos, sizeof(list) - pos, "%s%d", pos ? "," : "", i);
                if (pos >= (int)sizeof(list) - 4) break;
            }
        }
        MSLOG_INFO("AAC transcode STOP:  active=%d/%d, channels={%s}",
                   g_aac_active, RTMP_CTX_COUNT, pos ? list : "-");
    }
}

int send_aac_sequence_header_if_needed(aac_enc_ctx_t* A, struct rtmp_context_t* context, uint32_t pts_ms)
{
    if (!A->seq_hdr_sent) {
        int ret = flv_push_aac_packet(context, A->asc, A->asc_size, 0 /*seq hdr*/, pts_ms);
        if (ret != 0) { DVR_ERROR("push AAC seq header failed: %d", ret); return ret; }
        A->seq_hdr_sent = 1;
    }
    return 0;
}

void s16_to_flt(float* dst, const int16_t* src, int n)
{
    const float scale = 1.0f / 32768.0f;
    for (int i = 0; i < n; ++i) dst[i] = (float)src[i] * scale;
}

int encode_and_push_from_s16(aac_enc_ctx_t* A, struct rtmp_context_t* context)
{
    if (!A || !A->c) return -1;
    int ret = 0;

    const int need_in_8k = (A->sr_out == 16000) ? (A->enc_frame_size / 2) : A->enc_frame_size;

    while (A->s16fifo.size >= need_in_8k) {
        int16_t* in8k = (int16_t*)av_malloc(sizeof(int16_t) * need_in_8k);
        if (!in8k) return -1;
        if (s16_fifo_read(&A->s16fifo, in8k, need_in_8k) < need_in_8k) {
            av_free(in8k);
            DVR_ERROR("s16_fifo_read not enough");
            return -1;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) { av_free(in8k); DVR_ERROR("av_frame_alloc failed"); return -1; }
        frame->channels       = A->channels;
        frame->channel_layout = A->ch_layout;
        frame->format         = A->enc_fmt;
        frame->sample_rate    = A->sr_out;
        frame->nb_samples     = A->enc_frame_size;

        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame); av_free(in8k);
            DVR_ERROR("av_frame_get_buffer failed");
            return -1;
        }

        float* dst = (float*)frame->data[0];

        if (A->sr_out == 16000) {
            // 上采样 8k→16k
            int16_t* tmp16k = (int16_t*)av_malloc(sizeof(int16_t) * A->enc_frame_size);
            if (!tmp16k) { av_frame_free(&frame); av_free(in8k); return -1; }
            upsample2x_linear_s16(in8k, need_in_8k, tmp16k);
            s16_to_flt(dst, tmp16k, A->enc_frame_size);
            av_free(tmp16k);
        } else {
            // 直接 8k
            s16_to_flt(dst, in8k, A->enc_frame_size);
        }
        av_free(in8k);

        // PTS
        if (!A->pts_inited) {
            frame->pts = 0;
            A->pts_inited = 1;
        } else {
            frame->pts = av_rescale_q((int64_t)A->next_pts_ms, (AVRational){1,1000}, A->c->time_base);
        }

        if ((ret = avcodec_send_frame(A->c, frame)) < 0) {
            DVR_ERROR("avcodec_send_frame failed: %d", ret);
            av_frame_free(&frame);
            return -1;
        }
        av_frame_free(&frame);

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) { DVR_ERROR("av_packet_alloc failed"); return -1; }

        while (true) {
            ret = avcodec_receive_packet(A->c, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                ret = 0;
                break;
            }
            if (ret < 0) {
                DVR_ERROR("avcodec_receive_packet failed: %d", ret);
                av_packet_free(&pkt);
                return -1;
            }

            uint32_t pts_ms = A->next_pts_ms;
            int r = flv_push_aac_packet(context, pkt->data, pkt->size, 1 /*raw*/, pts_ms);
            av_packet_unref(pkt);
            if (r != 0) {
                DVR_ERROR("push AAC raw failed: %d", r);
                av_packet_free(&pkt);
                return -1;
            }

            A->next_pts_ms += A->frame_ms_step; // 8k:128ms, 16k:64ms
        }

        av_packet_free(&pkt);
    }

    return 0;
}

int transcode_g711_and_push(void* param, const unsigned char *buffer, unsigned int buffer_size, uint32_t pts_ms, int is_ulaw /*1=G711U, 0=G711A*/)
{
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;
    int ch = context->channel;

    if (!ch_ok(ch)) {
        DVR_ERROR("transcode_g711_and_push: ch=%d out of range [0..%d]", ch, RTMP_CTX_COUNT - 1);
        return 0;
    }

    if (aac_open_if_needed(ch, context) != 0) return -1;
    aac_enc_ctx_t* A = &g_aac[ch];

    if (!A->seq_hdr_sent) {
        A->next_pts_ms = pts_ms;
        if (send_aac_sequence_header_if_needed(A, context, A->next_pts_ms) != 0) return -1;

        {
            char list[4 * RTMP_CTX_COUNT + 8];
            int pos = 0;
            list[0] = '\0';
            for (int i = 0; i < RTMP_CTX_COUNT; ++i) {
                if (g_aac[i].initialized) {
                    pos += snprintf(list + pos, sizeof(list) - pos, "%s%d", pos ? "," : "", i);
                    if (pos >= (int)sizeof(list) - 4) break;
                }
            }
            MSLOG_INFO("AAC transcode START: active=%d/%d, channels={%s}, this_ch=%d, out_sr=%d",
                       g_aac_active, RTMP_CTX_COUNT, pos ? list : "-", ch, A->sr_out);
        }
    }

    int in_samples = (int)buffer_size; 
    if (in_samples <= 0) return 0;

    int16_t* pcm = (int16_t*)av_malloc(in_samples * sizeof(int16_t));
    if (!pcm) return -1;

    if (is_ulaw) {
        for (int i = 0; i < in_samples; ++i) pcm[i] = mulaw_decode(buffer[i]);
    } else {
        for (int i = 0; i < in_samples; ++i) pcm[i] = alaw_decode(buffer[i]);
    }

    int w = s16_fifo_write(&A->s16fifo, pcm, in_samples);
    av_free(pcm);
    if (w < 0) { DVR_ERROR("s16_fifo_write failed"); return -1; }

    return encode_and_push_from_s16(A, context);
}

#ifdef __cplusplus
}
#endif
