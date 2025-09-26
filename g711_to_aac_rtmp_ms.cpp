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
#include "g711_to_aac.h"
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
struct rtmp_ctrl_t rtmp_ctrl;


static inline int ms_is_adts(const unsigned char* p, unsigned int n) {
    return (n > 2) && (p[0] == 0xFF) && ((p[1] & 0xF0) == 0xF0);
}

enum {
    RTMP_CTX_COUNT = (int)(sizeof(((struct rtmp_ctrl_t*)0)->rtmp_ctx) / sizeof(((struct rtmp_ctrl_t*)0)->rtmp_ctx[0]))
};
#if RTMP_CTX_COUNT <= 0
#  undef RTMP_CTX_COUNT
#  define RTMP_CTX_COUNT 16
#endif

static inline bool ch_ok(int ch) { return ch >= 0 && ch < RTMP_CTX_COUNT; }

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

int ms_rtmp_write_audio_frame(void* param, unsigned char *buffer, unsigned int buffer_size, int64_t present_time) {
    uint32_t pts = (uint32_t)(present_time / 1000);
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;

    if (!ch_ok(context->channel)) return 0;

    if(context->audio_codec == MS_AUDIO_CODEC_G711U){
        return transcode_g711_and_push(param, buffer, buffer_size, pts, 1 /*ulaw*/);
    } else if(context->audio_codec == MS_AUDIO_CODEC_G711A){
        return transcode_g711_and_push(param, buffer, buffer_size, pts, 0 /*alaw*/);
    } else {
        return 0;
    }
}

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

static int rtmp_client_send(void* param, const void* header, size_t len, const void* data, size_t bytes)
{
    socket_t* socket = (socket_t*)param;
    socket_bufvec_t vec[2];
    socket_setbufvec(vec, 0, (void*)header, len);
    socket_setbufvec(vec, 1, (void*)data, bytes);

    return socket_send_v_all_by_time(*socket, vec, bytes > 0 ? 2 : 1, 0, 5000);
}

int ms_rtmp_context_init()
{
    rtmp_ctrl.g_rtmp_ms_run = 0;
    memset(&rtmp_ctrl, 0, sizeof(rtmp_ctrl));
    aac_reset_all();   
    return 0;
}

int ms_rtmp_audio_codec_changed(const char* codec)
{
    int ac = name_to_audio_codec_id(codec);
    for (int i = 0; i < RTMP_CTX_COUNT; ++i) {
        rtmp_ctrl.rtmp_ctx[i].audio_codec = ac;
    }
    MSLOG_INFO("All channels set audio codec=%s(%d)", codec ? codec : "G711U", ac);
    return 0;
}

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
