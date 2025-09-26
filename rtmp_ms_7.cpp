//实现本地8路+IPC8路音频转码并VLC拉流获取音频声音
#include "rtmp_ms.h"           
#include "flv-proto.h"
#include "ms_netdvr_vi.h"
#include "doordvr_export.h"
#include <sys/prctl.h>
#include "FramePackage.h"
#define RTMP_URL_LIVE_PLAY "rtmp://127.0.0.1/live"
#define RTMP_HOST_NAME "127.0.0.1"
#define RTMP_HOST_PORT 1935
#include "ms_netdvr_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#ifdef __linux__
#include <strings.h> // strcasecmp
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

// ====================== 全局控制结构（外部定义） ======================
struct rtmp_ctrl_t rtmp_ctrl;

// ========= 动态推导通道数 =========
enum {
    RTMP_CTX_COUNT = (int)(sizeof(((struct rtmp_ctrl_t*)0)->rtmp_ctx) /
                           sizeof(((struct rtmp_ctrl_t*)0)->rtmp_ctx[0]))
};
#if RTMP_CTX_COUNT <= 0
#  undef RTMP_CTX_COUNT
#  define RTMP_CTX_COUNT 16
#endif
static inline bool ch_ok(int ch) { return ch >= 0 && ch < RTMP_CTX_COUNT; }

static inline int ms_is_adts(const unsigned char* p, unsigned int n) {
    return (n > 2) && (p[0] == 0xFF) && ((p[1] & 0xF0) == 0xF0);
}

// ====================== G.711 解码（纯 C） ======================
static inline int16_t mulaw_decode(uint8_t u)
{
    u = ~u;
    int sign = (u & 0x80);
    int exponent = (u >> 4) & 0x07;
    int mantissa = (u & 0x0F);
    int sample = ((mantissa << 3) + 0x84) << exponent;
    sample -= 0x84;
    return (int16_t)(sign ? -sample : sample);
}
static inline int16_t alaw_decode(uint8_t a)
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

// ====================== 简单 S16 FIFO ======================
typedef struct {
    int16_t* buf;
    int      size;  // 已有样本数
    int      cap;   // 容量（样本数）
} s16_fifo_t;

static void s16_fifo_init(s16_fifo_t* F, int init_cap)
{
    memset(F, 0, sizeof(*F));
    if (init_cap < 1024) init_cap = 1024;
    F->buf = (int16_t*)av_malloc(sizeof(int16_t) * init_cap);
    F->cap = F->buf ? init_cap : 0;
    F->size = 0;
}
static void s16_fifo_free(s16_fifo_t* F)
{
    if (F->buf) av_free(F->buf);
    memset(F, 0, sizeof(*F));
}
static int s16_fifo_ensure(s16_fifo_t* F, int need_total)
{
    if (need_total <= F->cap) return 0;
    int new_cap = F->cap ? F->cap : 1024;
    while (new_cap < need_total) new_cap <<= 1;
    int16_t* nb = (int16_t*)av_realloc(F->buf, sizeof(int16_t) * new_cap);
    if (!nb) return -1;
    F->buf = nb; F->cap = new_cap;
    return 0;
}
static int s16_fifo_write(s16_fifo_t* F, const int16_t* in, int n)
{
    if (n <= 0) return 0;
    if (s16_fifo_ensure(F, F->size + n) != 0) return -1;
    memcpy(F->buf + F->size, in, sizeof(int16_t) * n);
    F->size += n;
    return n;
}
static int s16_fifo_read(s16_fifo_t* F, int16_t* out, int n)
{
    if (n <= 0 || F->size < n) return 0;
    memcpy(out, F->buf, sizeof(int16_t) * n);
    memmove(F->buf, F->buf + n, sizeof(int16_t) * (F->size - n));
    F->size -= n;
    return n;
}

// ====================== 每路 AAC 编码上下文 ======================
typedef struct {
    int initialized;

    AVCodec*        codec;
    AVCodecContext* c;

    // 采样率：sr_in 固定 8000；sr_out 为编码采样率（8k 或 16k）
    int             sr_in;          // 8000（G.711）
    int             sr_out;         // 8000/16000（由 rtmp_ctx[].aac_sr 决定）
    int             channels;       // 1
    int64_t         ch_layout;      // MONO
    AVSampleFormat  enc_fmt;        // FLTP
    int             enc_frame_size; // 编码器帧长（通常 1024）
    uint32_t        frame_ms_step;  // 每帧时间步长（ms）：1024/sr_out

    // extradata (ASC)
    uint8_t*        asc;
    int             asc_size;
    int             seq_hdr_sent;

    // PTS（ms）
    int             pts_inited;
    uint32_t        next_pts_ms;    // 8k→128ms/帧；16k→64ms/帧

    // 输入 S16 FIFO（仅存 8k 源）
    s16_fifo_t      s16fifo;
} aac_enc_ctx_t;

static aac_enc_ctx_t g_aac[RTMP_CTX_COUNT];
static int g_aac_active = 0;

// ========== codec 名字 → 内部枚举 ==========
static int name_to_audio_codec_id(const char* name)
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

// ========== 2×上采样（8k→16k）：纯 C 线性插值，恰好生成 2*N 个样本 ==========
static inline void upsample2x_linear_s16(const int16_t* in, int nin, int16_t* out /*size:2*nin*/)
{
    // 对每个输入样本插一个中点：out[2*i]=in[i]，out[2*i+1]=(in[i]+in[i+1])/2；
    // 最后一个的中点取自身（避免越界）。
    for (int i = 0; i < nin; ++i) {
        int16_t a = in[i];
        out[2*i] = a;
        int16_t b = (i+1 < nin) ? in[i+1] : in[i];
        out[2*i + 1] = (int16_t)((a + b) / 2);
    }
}

// ========== 发送 FLV AAC 包 ==========
static int flv_push_aac_packet(struct rtmp_context_t* context,
                               const uint8_t* payload, size_t payload_bytes,
                               uint8_t aac_packet_type, uint32_t pts_ms)
{
    if (!context || !context->rtmp_client || !context->start_rtmp) return 0;
    if (!ch_ok(context->channel)) return 0;

    // byte0: 0xAE (AAC/16bit/mono，SoundRate位对AAC忽略)
    uint8_t* out = (uint8_t*)av_malloc(2 + payload_bytes);
    if (!out) return -1;
    out[0] = 0xAE;
    out[1] = aac_packet_type; // 0=SeqHdr, 1=Raw
    if (payload_bytes > 0 && payload) memcpy(out + 2, payload, payload_bytes);

    int ret = rtmp_client_push_audio(context->rtmp_client, out, 2 + payload_bytes, pts_ms);
    av_free(out);
    return ret;
}

// ========== 打开 AAC 编码器 ==========
static int aac_open_if_needed(int ch, struct rtmp_context_t* context)
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

    A->sr_in   = 8000; // G.711 源固定 8k
    A->sr_out  = (context && context->aac_sr == 16000) ? 16000 : 8000; // 可按通道选择 16k
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

static void aac_close_if_open(int ch)
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

// ========== 发送 AAC SeqHdr（仅一次） ==========
static int send_aac_sequence_header_if_needed(aac_enc_ctx_t* A, struct rtmp_context_t* context, uint32_t pts_ms)
{
    if (!A->seq_hdr_sent) {
        int ret = flv_push_aac_packet(context, A->asc, A->asc_size, 0 /*seq hdr*/, pts_ms);
        if (ret != 0) { DVR_ERROR("push AAC seq header failed: %d", ret); return ret; }
        A->seq_hdr_sent = 1;
    }
    return 0;
}

// ========== S16 -> FLTP（单声道）辅助 ==========
static inline void s16_to_flt(float* dst, const int16_t* src, int n)
{
    const float scale = 1.0f / 32768.0f;
    for (int i = 0; i < n; ++i) dst[i] = (float)src[i] * scale;
}

// ========== 编码并推送 ==========
static int encode_and_push_from_s16(aac_enc_ctx_t* A, struct rtmp_context_t* context)
{
    if (!A || !A->c) return -1;
    int ret = 0;

    // 若输出 16k，则一次需要 512 个 8k 样本 → 上采样成 1024；否则直接 1024 个 8k 样本
    const int need_in_8k = (A->sr_out == 16000) ? (A->enc_frame_size / 2) : A->enc_frame_size;

    while (A->s16fifo.size >= need_in_8k) {
        // 取输入（8k）
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

// ============================= 原有回调：FLV 包推送 =============================
static int on_flv_packet(void* param, int type, const void* data, size_t bytes, uint32_t timestamp)
{
    int ret = 0;
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;

    if(context->start_rtmp == 0){
        DVR_DEBUG("on flv packet ========================\n");
        return 0;
    }

    if(FLV_TYPE_AUDIO == type){
        ret = rtmp_client_push_audio(context->rtmp_client, data, bytes, timestamp);
    }
    else if(FLV_TYPE_VIDEO == type){
        ret = rtmp_client_push_video(context->rtmp_client, data, bytes, timestamp);
    }
    else if (FLV_TYPE_SCRIPT == type)
    {
        ret = rtmp_client_push_script(context->rtmp_client, data, bytes, timestamp);
    }
    else
    {
        assert(0);
        ret = 0; // ignore
    }

    if (0 != ret)
    {
        assert(0);
        // TODO: handle send failed
    }

    return ret;
}

// ============================= 视频路径不变 =============================
int ms_rtmp_write_video_frame(void* param, unsigned char *buffer, unsigned int buffer_size, int64_t present_time) {
    uint32_t pts = (uint32_t)(present_time / 1000);
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;

    if (!ch_ok(context->channel)) return 0;

    if(context->video_codec == MS_MODTYPE_H264){
        return flv_muxer_avc(context->flv_muxer, buffer, buffer_size, pts, pts);
    }else if(context->video_codec == MS_MODTYPE_H265){
        return flv_muxer_hevc(context->flv_muxer, buffer, buffer_size, pts, pts);
    }
    return 0;
}

// ====================== 音频路径：G711(U/A) → PCM16 → (可选上采样) → FLTP → AAC ======================
static unsigned s_audio_sent = 0;

static int transcode_g711_and_push(void* param,
                                   const unsigned char *buffer, unsigned int buffer_size,
                                   uint32_t pts_ms, int is_ulaw /*1=G711U, 0=G711A*/)
{
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;
    int ch = context->channel;

    if (!ch_ok(ch)) {
        DVR_ERROR("transcode_g711_and_push: ch=%d out of range [0..%d]", ch, RTMP_CTX_COUNT - 1);
        return 0;
    }

    if (aac_open_if_needed(ch, context) != 0) return -1;
    aac_enc_ctx_t* A = &g_aac[ch];

    // 首帧：记录起始 PTS 并发 AAC SeqHdr
    if (!A->seq_hdr_sent) {
        A->next_pts_ms = pts_ms;
        if (send_aac_sequence_header_if_needed(A, context, A->next_pts_ms) != 0) return -1;

        // —— 打印“当前有几路在转AAC，以及通道列表”
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

    // 将 G.711 字节流解码到临时 S16（8k），再写入 FIFO
    int in_samples = (int)buffer_size; // 1字节=1样本（G.711）
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

    // 编码并推送
    return encode_and_push_from_s16(A, context);
}

int ms_rtmp_write_audio_frame(void* param, unsigned char *buffer, unsigned int buffer_size, int64_t present_time) {
    uint32_t pts = (uint32_t)(present_time / 1000);
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;

    if (!ch_ok(context->channel)) return 0;

    if ((++s_audio_sent % 60) == 0) {
        if (ms_is_adts(buffer, buffer_size)) {
            MSLOG_INFO("RTMP audio: detected ADTS(AAC) len=%u pts=%u", buffer_size, pts);
        } else {
            MSLOG_INFO("RTMP audio: ch:%d enum codec=%d (G711A=%d,G711U=%d,G726=%d), len=%u pts=%u",
                       context->channel, context->audio_codec, MS_AUDIO_CODEC_G711A, MS_AUDIO_CODEC_G711U, MS_AUDIO_CODEC_G726,
                       buffer_size, pts);
        }
    }

    if(context->audio_codec == MS_AUDIO_CODEC_G711U){
        return transcode_g711_and_push(param, buffer, buffer_size, pts, 1 /*ulaw*/);
    } else if(context->audio_codec == MS_AUDIO_CODEC_G711A){
        return transcode_g711_and_push(param, buffer, buffer_size, pts, 0 /*alaw*/);
    } else {
        // 其它编码则原样忽略/上层自行处理
        return 0;
    }
}

// ============================= 采集线程（仅边界保护） =============================
void *flv_muxer_thread(void *param) {

    char thread_name[32] = {0};
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;
    if (!ch_ok(context->channel)) return NULL;

    sprintf(thread_name,"flv_muxer_thread_%d",context->channel);
    DVR_DEBUG("thread %s start... !!!",thread_name);
    prctl(PR_SET_NAME, thread_name, 0, 0, 0);

    int ret = 0;
    int frame_num = 0;
    unsigned char *frame_data = NULL;
    FRAME_INFO_EX frame_info;

    while (context->start_rtmp) {
        frame_num = GetFrameNumbers(context->channel, 1);
        if(frame_num > 0){
            ret = Hqt_Mpi_GetVideoFrame(context->channel, 1, &frame_info, &frame_data);
            if(context->reference < 1){
                Hqt_Mpi_Delete(frame_data);
                usleep(1000*50);
                continue;
            }

            if(ret){
                if(frame_info.frameType == 0 || frame_info.frameType == 1){//视频帧
                    ms_rtmp_write_video_frame(param, frame_data, frame_info.length, frame_info.relativeTime);
                    Hqt_Mpi_Delete(frame_data);
                }
                else if(frame_info.frameType == 2){//音频帧（本地/IPC 统一）
                    ms_rtmp_write_audio_frame(param, frame_data, frame_info.length, frame_info.relativeTime);
                    Hqt_Mpi_Delete(frame_data);
                }else{
                    Hqt_Mpi_Delete(frame_data);
                }
            }

            usleep(1000*10);
        }
        else{
            usleep(1000*100);
        }
    }
    DVR_DEBUG("thread %s exit !!!",thread_name);

    return NULL;
}

// ============================= RTMP 发送函数不变 =============================
static int rtmp_client_send(void* param, const void* header, size_t len, const void* data, size_t bytes)
{
    socket_t* socket = (socket_t*)param;
    socket_bufvec_t vec[2];
    socket_setbufvec(vec, 0, (void*)header, len);
    socket_setbufvec(vec, 1, (void*)data, bytes);

    return socket_send_v_all_by_time(*socket, vec, bytes > 0 ? 2 : 1, 0, 5000);
}

// ============================= 初始化/反初始化 =============================
int ms_rtmp_context_init()
{
    rtmp_ctrl.g_rtmp_ms_run = 0;
    memset(&rtmp_ctrl, 0, sizeof(rtmp_ctrl));
    for (int i=0; i<RTMP_CTX_COUNT; ++i) memset(&g_aac[i], 0, sizeof(g_aac[i]));
    return 0;
}

// 统一设置全部通道音频编码类型（G711A/G711U），来自外部配置
int ms_rtmp_audio_codec_changed(const char* codec)
{
    int ac = name_to_audio_codec_id(codec);
    for (int i = 0; i < RTMP_CTX_COUNT; ++i) {
        rtmp_ctrl.rtmp_ctx[i].audio_codec = ac;
    }
    MSLOG_INFO("All channels set audio codec=%s(%d)", codec ? codec : "G711U", ac);
    return 0;
}

// 只设置某一路（用于 IPC 按实际 SDP 协议设置 PCMU/PCMA）
int ms_rtmp_set_channel_audio_codec(int channel, const char* codec_name)
{
    if (!ch_ok(channel)) return -1;
    int ac = name_to_audio_codec_id(codec_name);
    rtmp_ctrl.rtmp_ctx[channel].audio_codec = ac;
    MSLOG_INFO("Set channel %d audio codec=%s(%d)", channel, codec_name ? codec_name : "G711U", ac);
    return 0;
}

int ms_rtmp_video_codec_changed(int channel,const char* /*codec*/)
{
    if (!ch_ok(channel)) return -1;
    const char *encode_type = "H.264";
    if (!strcmp(encode_type, "H.264"))
        rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H264;
    else if(!strcmp(encode_type, "H.265"))
        rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H265;
    return 0;
}

int ms_rtmp_stream_event(char* addr, char* stream, E_RTMP_PLAY_EVENT_FLAG playflag)
{
    if((addr == NULL)||(stream == NULL)){
        DVR_DEBUG("invalid addr ||stream value !!!");
        return -1;
    }

    if(playflag == RTMP_PLAY_EVENT_FLAG_PLAY){
        if(!strcmp(stream,"mainstream")){
            rtmp_ctrl.rtmp_ctx[0].reference++;
        }else if(!strcmp(stream,"substream")){
            rtmp_ctrl.rtmp_ctx[1].reference++;
        }else if(!strcmp(stream,"thirdstream")){
            rtmp_ctrl.rtmp_ctx[2].reference++;
        }else if(!strcmp(stream,"playback")){
            rtmp_ctrl.rtmp_ctx[3].reference++;
        }
    }else if(playflag == RTMP_PLAY_EVENT_FLAG_DONE){
        if(!strcmp(stream,"mainstream")){
            if(rtmp_ctrl.rtmp_ctx[0].reference > 0) rtmp_ctrl.rtmp_ctx[0].reference--;
        }else if(!strcmp(stream,"substream")){
            if(rtmp_ctrl.rtmp_ctx[1].reference > 0) rtmp_ctrl.rtmp_ctx[1].reference--;
        }else if(!strcmp(stream,"thirdstream")){
            if(rtmp_ctrl.rtmp_ctx[2].reference > 0) rtmp_ctrl.rtmp_ctx[2].reference--;
        }else if(!strcmp(stream,"playback")){
            if(rtmp_ctrl.rtmp_ctx[3].reference > 0) rtmp_ctrl.rtmp_ctx[3].reference--;
        }
    }else{
        DVR_DEBUG("invalid playflag value !!!");
    }
    return 0;
}

int ms_rtmp_channel_init(int channel, const char* streamname)
{
    if (!ch_ok(channel)) {
        DVR_ERROR("ms_rtmp_channel_init: ch=%d out of range [0..%d]", channel, RTMP_CTX_COUNT - 1);
        return -1;
    }

    char packet[2 * 1024] = {0};

    rtmp_ctrl.g_rtmp_ms_run = 1;

    if(rtmp_ctrl.rtmp_ctx[channel].start_rtmp > 0){
        DVR_DEBUG("already initialized !!!");
        return 0;
    }

    memset(&rtmp_ctrl.rtmp_ctx[channel], 0, sizeof(struct rtmp_context_t));

    // 音频：默认按 G711U（PCMU），IPC 会在 live555 的 SETUP 回调里按实际改为 G711U/G711A
    const char *encode_type_a = "G711U";
    rtmp_ctrl.rtmp_ctx[channel].audio_codec =
        (!strcmp(encode_type_a, "G711A")) ? MS_AUDIO_CODEC_G711A : MS_AUDIO_CODEC_G711U;

    // AAC 目标采样率默认 8000（若某路想 16k，请在外部把 rtmp_ctx[ch].aac_sr 设为 16000）
    if (rtmp_ctrl.rtmp_ctx[channel].aac_sr != 16000)
        rtmp_ctrl.rtmp_ctx[channel].aac_sr = 8000;

    const char *encode_type_v = "H.264";
    if (!strcmp(encode_type_v, "H.264"))
        rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H264;
    else if(!strcmp(encode_type_v, "H.265"))
        rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H265;

    rtmp_ctrl.rtmp_ctx[channel].channel   = channel;
    rtmp_ctrl.rtmp_ctx[channel].flv_muxer = flv_muxer_create(on_flv_packet, &rtmp_ctrl.rtmp_ctx[channel]);
    if(rtmp_ctrl.rtmp_ctx[channel].flv_muxer == NULL){
        DVR_DEBUG("flv muxer create failed !!! \n");
    }
    rtmp_ctrl.rtmp_ctx[channel].start_rtmp = 0;
    rtmp_ctrl.rtmp_ctx[channel].reference  = 100;

    struct rtmp_client_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.send = rtmp_client_send;

    socket_init();
    rtmp_ctrl.rtmp_ctx[channel].socket = socket_connect_host(RTMP_HOST_NAME, RTMP_HOST_PORT, 2000);
    socket_setnonblock(rtmp_ctrl.rtmp_ctx[channel].socket, 0);

    rtmp_ctrl.rtmp_ctx[channel].rtmp_client = rtmp_client_create("live",streamname,RTMP_URL_LIVE_PLAY, &rtmp_ctrl.rtmp_ctx[channel].socket, &handler);
    int r = rtmp_client_start(rtmp_ctrl.rtmp_ctx[channel].rtmp_client, 0);
    DVR_DEBUG("rtmp start ************************888888888888*********** :%d \n",r);
    while (4 != rtmp_client_getstate(rtmp_ctrl.rtmp_ctx[channel].rtmp_client) && (r = socket_recv(rtmp_ctrl.rtmp_ctx[channel].socket, packet, sizeof(packet), 0)) > 0)
    {
        if(rtmp_client_input(rtmp_ctrl.rtmp_ctx[channel].rtmp_client, packet, r) != 0 ){
            DVR_DEBUG("=============error=========================\n");
        }
    }

    DVR_DEBUG("rtmp start *********************************** :%d  %d \n",rtmp_client_getstate(rtmp_ctrl.rtmp_ctx[channel].rtmp_client),r);
    if (rtmp_client_getstate(rtmp_ctrl.rtmp_ctx[channel].rtmp_client) != 4) {
        DVR_ERROR("rtmp not ready, state=%d",
                  rtmp_client_getstate(rtmp_ctrl.rtmp_ctx[channel].rtmp_client));
        rtmp_client_destroy(rtmp_ctrl.rtmp_ctx[channel].rtmp_client);
        socket_close(rtmp_ctrl.rtmp_ctx[channel].socket);
        flv_muxer_destroy(rtmp_ctrl.rtmp_ctx[channel].flv_muxer);
        return -1;
    }
    rtmp_ctrl.rtmp_ctx[channel].start_rtmp = 1;
    pthread_create(&rtmp_ctrl.rtmp_ctx[channel].flv_muxer_tid, NULL, flv_muxer_thread, &rtmp_ctrl.rtmp_ctx[channel]);

    return 0;
}

int  ms_rtmp_channel_deinit(int channel)
{
    if (!ch_ok(channel)) return -1;

    rtmp_ctrl.g_rtmp_ms_run = 0;
    rtmp_ctrl.rtmp_ctx[channel].start_rtmp = 0;
    pthread_join(rtmp_ctrl.rtmp_ctx[channel].flv_muxer_tid, NULL);

    // 释放 AAC 编码资源
    aac_close_if_open(channel);

    flv_muxer_destroy(rtmp_ctrl.rtmp_ctx[channel].flv_muxer);
    rtmp_client_destroy(rtmp_ctrl.rtmp_ctx[channel].rtmp_client);
    socket_close(rtmp_ctrl.rtmp_ctx[channel].socket);
    socket_cleanup();

    return 0;
}
