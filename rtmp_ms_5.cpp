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
struct rtmp_ctrl_t rtmp_ctrl;



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
       //return flv_writer_input(context->flv_file, type, data, bytes, timestamp);//写入测试文件
}



int ms_rtmp_write_video_frame(void* param, unsigned char *buffer, unsigned int buffer_size, int64_t present_time) {
    uint32_t pts = (uint32_t)(present_time / 1000); 
    struct rtmp_context_t *context = (struct rtmp_context_t*)param;

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

               if(context->audio_codec == MS_AUDIO_CODEC_G711A){//AUDIO_CODEC_G711A
               return flv_muxer_g711a(context->flv_muxer, buffer, buffer_size, pts, pts);
               }else if(context->audio_codec == MS_AUDIO_CODEC_G711U){//AUDIO_CODEC_G711U
      return flv_muxer_g711u(context->flv_muxer, buffer, buffer_size, pts, pts);
               }
               return 0;
}


void *flv_muxer_thread(void *param) {

  char thread_name[32] = {0};
       struct rtmp_context_t *context = (struct rtmp_context_t*)param;
       sprintf(thread_name,"flv_muxer_thread_%d",context->channel);
       DVR_DEBUG("thread %s start... !!!",thread_name);
       prctl(PR_SET_NAME, thread_name, 0, 0, 0);
  int ret = 0;
  int frame_num = 0;
  unsigned char *frame_data = NULL;
  FRAME_INFO_EX frame_info;
       //int frame_sent = 0;
  //unsigned char *video_frame = NULL;
  //unsigned char *audio_frame = NULL;
  //ENCODER_FRAME_INFO_S video_frame_info;
  //ENCODER_FRAME_INFO_S audio_frame_info;
  
  while (context->start_rtmp) { 
               //DVR_DEBUG("context channel %d \n",context->channel);
     frame_num = GetFrameNumbers(context->channel, 1);
          //DVR_DEBUG("context channel %d  frame num :%d \n",context->channel,frame_num);
     if(frame_num > 0){
ret = Hqt_Mpi_GetVideoFrame(context->channel, 1, &frame_info, &frame_data);
                   if(context->reference < 1){
                                        Hqt_Mpi_Delete(frame_data);
                                        usleep(1000*50);
           continue;
                               }
                               
        if(ret){

            if(frame_info.frameType == 0 || frame_info.frameType == 1){//视频帧
            
              //video_frame_info = frame_info;
              //video_frame = frame_data;
                                                       //ms_rtmp_write_video_frame(0, video_frame, video_frame_info.u32Length,video_frame_info.u64RelativeTime);
                                                 ms_rtmp_write_video_frame(param, frame_data, frame_info.length,frame_info.relativeTime);
                                                       Hqt_Mpi_Delete(frame_data);
            }
            else if(frame_info.frameType == 2){//音频帧
              //audio_frame_info = frame_info;
              //audio_frame = frame_data;
              //ms_rtmp_write_audio_frame(0, audio_frame, audio_frame_info.u32Length, audio_frame_info.u64RelativeTime);
                                                 ms_rtmp_write_audio_frame(param, frame_data, frame_info.length, frame_info.relativeTime);
                                                 Hqt_Mpi_Delete(frame_data);

            }else{
              //printf("=============flv_muxer_thread==================else=======ret:%d \n",ret);
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
        return 0;
}

int ms_rtmp_audio_codec_changed(const char* codec)
{

//      const char *encode_type = "G711U";//rk_param_get_string("audio.0:encode_type", "G711A");
//        if (!strcmp(encode_type, "G711A")){
//                rtmp_ctrl.rtmp_ctx[0].audio_codec = MS_AUDIO_CODEC_G711A;
//                rtmp_ctrl.rtmp_ctx[1].audio_codec = MS_AUDIO_CODEC_G711A;
//          rtmp_ctrl.rtmp_ctx[2].audio_codec = MS_AUDIO_CODEC_G711A;

//        }else if(!strcmp(encode_type, "G711U")){
//     rtmp_ctrl.rtmp_ctx[0].audio_codec = MS_AUDIO_CODEC_G711U;
//                rtmp_ctrl.rtmp_ctx[1].audio_codec = MS_AUDIO_CODEC_G711U;
//                rtmp_ctrl.rtmp_ctx[2].audio_codec = MS_AUDIO_CODEC_G711U;
//        }
//        return 0;

    const char *encode_type = "G711U";
    int ac = (!strcmp(encode_type, "G711A")) ? MS_AUDIO_CODEC_G711A : MS_AUDIO_CODEC_G711U;
    for (int i = 0; i < DVR_ALL_CH_NUM; ++i) {
        rtmp_ctrl.rtmp_ctx[i].audio_codec = ac;
    }
    return 0;
}


int ms_rtmp_video_codec_changed(int channel,const char* codec)
{
       char entry[128] = {'\0'};
       const char *encode_type;

       snprintf(entry, 127, "video.%d:output_data_type", channel);
       encode_type = "H.264"; //rk_param_get_string(entry, "H.264");
       if (!strcmp(encode_type, "H.264"))
       {
               rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H264;
       }
       else if(!strcmp(encode_type, "H.265"))
       {
               rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H265;
       }
       return 0;
}

int ms_rtmp_stream_event(char* addr, char* stream, E_RTMP_PLAY_EVENT_FLAG playflag)
{
   int i = 0;

   DVR_DEBUG("====================addr:%s=========stream:%s=============\n",addr,stream);
        
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
                          if(rtmp_ctrl.rtmp_ctx[0].reference > 0){
                rtmp_ctrl.rtmp_ctx[0].reference--;
                                }
                       }else if(!strcmp(stream,"substream")){
                                        if(rtmp_ctrl.rtmp_ctx[1].reference > 0){
                                                rtmp_ctrl.rtmp_ctx[1].reference--;
                                        }
                       }else if(!strcmp(stream,"thirdstream")){
                                if(rtmp_ctrl.rtmp_ctx[2].reference > 0){
                                        rtmp_ctrl.rtmp_ctx[2].reference--;
                                }
                       }else if(!strcmp(stream,"playback")){
                                if(rtmp_ctrl.rtmp_ctx[3].reference > 0){
                                        rtmp_ctrl.rtmp_ctx[3].reference--;
                                }
                       }

        }else{

     DVR_DEBUG("invalid playflag value !!!");
        }

return 0;
}

int ms_rtmp_channel_init(int channel, const char* streamname)
{
       char packet[2 * 1024] = {0};
       char entry[128] = {'\0'};
       const char *encode_type;
       
  rtmp_ctrl.g_rtmp_ms_run = 1;

  if((channel < (sizeof(rtmp_ctrl.rtmp_ctx)/sizeof(struct rtmp_context_t)))){
               if(rtmp_ctrl.rtmp_ctx[channel].start_rtmp > 0){//如果已经初始化过了直接退出
                  DVR_DEBUG("already initialized !!!");
       return 0;
               }

       }else{
         DVR_DEBUG("invalid channel value !!!");
    return -1;
       }

       memset(&rtmp_ctrl.rtmp_ctx[channel], 0, sizeof(struct rtmp_context_t));
       //rtmp_ctrl.rtmp_ctx[channel].flv_file = flv_writer_create(NULL);
  //if(rtmp_ctrl.rtmp_ctx[channel].flv_file == NULL){
  //  printf("flv writer create failed !!! \n");
       //}
  encode_type = "G711U"; //rk_param_get_string("audio.0:encode_type", "G711A");
       if (!strcmp(encode_type, "G711A")){
               rtmp_ctrl.rtmp_ctx[channel].audio_codec = MS_AUDIO_CODEC_G711A;

       }else if(!strcmp(encode_type, "G711U")){
    rtmp_ctrl.rtmp_ctx[channel].audio_codec = MS_AUDIO_CODEC_G711U;
       }

       snprintf(entry, 127, "video.%d:output_data_type", channel);
       encode_type = "H.264";//rk_param_get_string(entry, "H.264");
       if (!strcmp(encode_type, "H.264"))
       {
               rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H264;
       }
       else if(!strcmp(encode_type, "H.265"))
       {
               rtmp_ctrl.rtmp_ctx[channel].video_codec = MS_MODTYPE_H265;
       }

       

       rtmp_ctrl.rtmp_ctx[channel].channel = channel;
       rtmp_ctrl.rtmp_ctx[channel].flv_muxer = flv_muxer_create(on_flv_packet, &rtmp_ctrl.rtmp_ctx[channel]);

       if(rtmp_ctrl.rtmp_ctx[channel].flv_muxer == NULL){
    DVR_DEBUG("flv muxer create failed !!! \n");
       }
  rtmp_ctrl.rtmp_ctx[channel].start_rtmp = 0;
       rtmp_ctrl.rtmp_ctx[channel].reference = 100;

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
  rtmp_ctrl.g_rtmp_ms_run = 0;
       rtmp_ctrl.rtmp_ctx[channel].start_rtmp = 0;
  pthread_join(rtmp_ctrl.rtmp_ctx[channel].flv_muxer_tid, NULL);
  flv_muxer_destroy(rtmp_ctrl.rtmp_ctx[channel].flv_muxer);
       //flv_writer_destroy(rtmp_ctrl.rtmp_ctx[channel].flv_file);

  rtmp_client_destroy(rtmp_ctrl.rtmp_ctx[channel].rtmp_client);
  socket_close(rtmp_ctrl.rtmp_ctx[channel].socket);
  socket_cleanup();
       
  return 0;
}
