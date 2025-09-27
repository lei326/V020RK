#include "flv-writer.h"
#include "flv-muxer.h"
#include "mpeg4-avc.h"
#include "rtmp-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "sockutil.h"
#include "sys/system.h"
#include "g711_to_aac.h"
struct rtmp_context_t
{
	flv_muxer_t* flv_muxer;
	rtmp_client_t* rtmp_client;
	socket_t socket;
	void* flv_file;
	int start_rtmp;
	int channel;
	int audio_codec;   // 2- g711a 3-g711u
	int video_codec;   // 0- h264  1-h265
	int reference;
	int        aac_sr; // AAC 目标采样率8k 或 16k 
	pthread_t  flv_muxer_tid;
};

struct rtmp_ctrl_t
{
  int g_rtmp_ms_run;
  struct rtmp_context_t rtmp_ctx[16];
};

typedef enum{
 RTMP_PLAY_EVENT_FLAG_DONE = 0,
 RTMP_PLAY_EVENT_FLAG_PLAY = 1,
 RTMP_PLAY_EVENT_FLAG_BUTT = 2
}E_RTMP_PLAY_EVENT_FLAG;

int ms_rtmp_audio_codec_changed(const char* codec);

int ms_rtmp_video_codec_changed(int channel,const char* codec);

int ms_rtmp_stream_event(char* addr, char* stream, E_RTMP_PLAY_EVENT_FLAG playflag);


int ms_rtmp_write_video_frame(void* param, unsigned char *buffer, unsigned int buffer_size, int64_t present_time);

int ms_rtmp_write_audio_frame(void* param, unsigned char *buffer, unsigned int buffer_size, int64_t present_time);

int ms_rtmp_context_init();

int ms_rtmp_channel_init(int channel, const char* streamname);

int ms_rtmp_channel_deinit(int channel);

int ms_rtmp_set_channel_audio_codec(int channel, const char* codec_name);
