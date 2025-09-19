#include "doordvr_export.h"
#include "FramePackage.h"
#include "my_pub_common.h"
#ifdef DMALLOC
#include <dmalloc.h>
#endif
#include "rtmp_ms.h"
static DVR_I32_T channel_stream[DVR_ALL_CH_NUM];
static pthread_mutex_t enc_lock[DVR_ALL_CH_NUM];
//static CMsMutex m_Lock;
static DVR_U32_T stream_statistics_count[DVR_ALL_CH_NUM];
static long long stream_statistics_time[DVR_ALL_CH_NUM];

DVR_I32_T DVR_CreateH264DecStream(DVR_U8_T chn)
{
	pthread_t id;
	PTHREAD_PARAM_T *pParam;
	
	pParam=ComGetPthreadParamAddr((PTHREAD_ID)(PTHREAD_MAIN_IDENC1_ID + chn));
  pParam->states=STATE_RUNNING;
  int* channel = (int*)malloc(sizeof(int));
  *channel = chn;
  id=ComCreateThread(pParam, channel,DVR_H264_StreamServer);
  if(id==0)
  {
    MSLOG_DEBUG("create decStream pthread fail");
    exit(-1);
  }

	return 0;
}

/***********************************************************************************************************
**函数:
**功能:
**输入参数:
**返回值:
***********************************************************************************************************/
DVR_I32_T   DVR_CreateH264MediaStreamServer(DVR_H264_Profile_T *DTH264Profile)
{
#ifdef  CONFIG_STREAMSERVER
  int i = 0;
  
  

  bzero(&channel_stream,sizeof(DVR_I32_T)*DVR_ALL_CH_NUM);
  for (i=0; i<DVR_ALL_CH_NUM; i++)
  {
    pthread_mutex_init(&enc_lock[i], NULL);
  }
#ifdef CONFIG_OSD
  SYS_Osd_Init();
#endif
  for(i = 0; i < DVR_ALL_CH_NUM; i++)
  {
    DVR_CreateH264DecStream(i);
  }
#ifdef SUPPORT_NET_TTX
  for(; i<DVR_ALL_CH_NUM; i++)
  {
    DVR_CreateH264DecStream(i);
  }
#endif
#endif
  return 0;
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
DVR_I32_T   DVR_DestroyH264MediaStreamServer(void)
{
#ifdef  CONFIG_STREAMSERVER
  int i = 0;
  PTHREAD_PARAM_T *pParam;
  for(i = 0; i < DVR_ALL_CH_NUM; i++)
  {
    pParam=ComGetPthreadParamAddr((PTHREAD_ID)(PTHREAD_MAIN_IDENC1_ID+i));
    ComExitThread(pParam);
  }
#ifdef SUPPORT_NET_TTX
  for(; i < DVR_ALL_CH_NUM; i++)
  {
    pParam=ComGetPthreadParamAddr(PTHREAD_MAIN_IDENC1_ID+i);
    ComExitThread(pParam);
  }
#endif
  for (i=0; i<DVR_ALL_CH_NUM; i++)
  {
    pthread_mutex_destroy(&enc_lock[i]);
  }
#ifdef CONFIG_OSD
  SYS_Osd_UnInit();
#endif
#endif
  return 0;
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
DVR_I32_T   DVR_PauseH264MediaStreamServer(void)
{
#ifdef  CONFIG_STREAMSERVER
  PTHREAD_PARAM_T *pParam;
  pParam=ComGetPthreadParamAddr(PTHREAD_MAIN_IDENC1_ID);
  ComPauseThread(pParam);
#endif
  return 0;
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
DVR_I32_T   DVR_RunH264MediaStreamServer(void)
{
#ifdef  CONFIG_STREAMSERVER
  PTHREAD_PARAM_T *pParam;
  pParam=ComGetPthreadParamAddr(PTHREAD_MAIN_IDENC1_ID);
  ComRunThread(pParam);
#endif
  return 0;
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
void GetVideoLock(void)
{
  for(int i=0; i < DVR_ALL_CH_NUM; i++)
    pthread_mutex_lock(&enc_lock[i]);
}
void ReleaseVideoLock(void)
{
  for(int i=0; i < DVR_ALL_CH_NUM; i++)
    pthread_mutex_unlock(&enc_lock[i]);
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
void AddGetChStream(DVR_U8_T ch)
{
  pthread_mutex_lock(&enc_lock[ch]);
  channel_stream[ch]=1;
  pthread_mutex_unlock(&enc_lock[ch]);
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
void RemoveGetChStream(DVR_U8_T ch)
{
  pthread_mutex_lock(&enc_lock[ch]);
  channel_stream[ch]=0;
  pthread_mutex_unlock(&enc_lock[ch]);
}
/***********************************************************************************************************
**函数:
**功能:
**输入参数:
**返回值:
***********************************************************************************************************/

void *DVR_H264_StreamServer(void *vpARG)
{
#ifdef  CONFIG_STREAMSERVER
  DVR_I32_T i=0;
  struct timeval TimeoutVal;

  DVR_I32_T s32ret;
  DVR_U32_T  dwFrameNumber = 0;

  DVR_I32_T VeChannel=*(DVR_I32_T*)vpARG ;

  DVR_U8_T FrameType;
  DVR_U32_T buf_size;
  DVR_I32_T maxfd = 0;
  long long  current_time;
  char encparam_count=0;
  long long osd_startTime=0,osd_endTime=0;

  PTHREAD_PARAM_T *pThreadParam ;
  int quit=0;;
  int states=STATE_UNKNOW;
  FRAME_INFO_T frame_info;
  _H264_ENC_PARAM_T enc_coder_param,enc_param;
  MEDIA_BUFFER mb = NULL;
  DVR_U32_T err_count = 0;

  if (vpARG == NULL)
  {
    MSLOG_ERROR(" DVR_H264_StreamServer  ---------error");
    return NULL;
  }

  //MSLOG_DEBUG("h264 stream server thread running........");
  MSLOG_DEBUG("start h264 stream %d server thread running........thread pid = %u",VeChannel,(unsigned int)syscall(224));

  pThreadParam=ComGetPthreadParamAddr((PTHREAD_ID)(PTHREAD_MAIN_IDENC1_ID + VeChannel));
  AddGetChStream(VeChannel);
#ifdef CONFIG_OSD
  if (VeChannel < DOORDVR_CHANNEL_NUM) {
    Hqt_Osd_InitRegions(VeChannel);
    SYS_Osd_Create(VeChannel);
  }
#endif

	
		 char name[8];
		 memset(name,0,sizeof(name));
	   sprintf(name,"ch%02d",VeChannel);
		 DVR_DEBUG("rtmp stream server thread running........%s ",name);
	   ms_rtmp_channel_init(VeChannel, name);
	
    DVR_DEBUG("h264 stream server thread running........%d ",pThreadParam->states);
  while (!quit)
  {
    switch (pThreadParam->states)
    {
      case STATE_PAUSED:
      {
        if (states!=STATE_PAUSED)
        {
          states=STATE_PAUSED;
        }
        MSLOG_DEBUG("h264 stream server new state:STATE_PAUSED");
        ComMutexCond_Notice_Signal(pThreadParam);
        ComMutexCond_Wait(true,pThreadParam);
      }
      break;
      case STATE_STOPPED:
      {
        MSLOG_DEBUG("preview thread  new state:STATE_STOPPED");
        ComMutexCond_Notice_Signal(pThreadParam);
        ComMutexCond_Wait(true,pThreadParam);
        break;
      }
      case STATE_INITIAL:
      {
        MSLOG_DEBUG("preview thread new state:STATE_INITIAL");
        quit=1;
        break;
      }
      case STATE_RUNNING:
      {
        if (states!=STATE_RUNNING)
        {
          MSLOG_DEBUG("h264 stream server  new state:STATE_RUNNING");
          states=STATE_RUNNING;
        }

        if (pThreadParam->states!=STATE_RUNNING)
          break;

        current_time=local_get_curtime();
        osd_endTime=current_time;
        if(osd_startTime==0)
        {
          osd_startTime=osd_endTime;
        }

        if (VeChannel >= DOORDVR_CHANNEL_NUM) {
  usleep(10000);   
  break;          
}

        mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, VeChannel, 2000);
        if (!mb)
        {
          MSLOG_DEBUG("RK_MPI_SYS_GetMediaBuffer get null buffer! channel = %d\n",VeChannel);
          err_count++;
          if(err_count > 2)
          {
            DVR_SYSTEM_INFO_T *sysInfo;
            MSLOG_DEBUG("RK_MPI_SYS_GetMediaBuffer get null buffer! by channel(%d) POWER_REBOOT",VeChannel);
            sysInfo = SystemGlobal_GetSystemGlobalContext();				
            //sysInfo->RbootfOrPowerDown = POWER_REBOOT;
						sysInfo->RequireReStartVenc[VeChannel] = 1;
						quit=1;
        		break;
          }
          continue;
        }
        else
        {
          err_count = 0;				
        }

        bzero(&frame_info,sizeof(FRAME_INFO_T));
        if (RK_MPI_MB_GetFlag(mb) == 4)
          frame_info.keyFrame=1;
        frame_info.channel=VeChannel%DOORDVR_CHANNEL_NUM;
        if(frame_info.keyFrame == 1)
          frame_info.frameType = 0;		//FRAME_TYPE_H264I;
        else
          frame_info.frameType = 1;		//FRAME_TYPE_H264P;
        frame_info.frame_id=dwFrameNumber++;
        MPI_VENC_GetCoderParam(VeChannel,&enc_coder_param);
        frame_info.width= enc_coder_param.dstWidth;
        frame_info.height=enc_coder_param.dstHeight;
        frame_info.time=current_time;
        frame_info.relativeTime = RK_MPI_MB_GetTimestamp(mb);
        frame_info.length=RK_MPI_MB_GetSize(mb);
        frame_info.pData=(unsigned char*)RK_MPI_MB_GetPtr(mb);
        frame_info.encoder_format = enc_coder_param.encoder_format;


        pthread_mutex_lock(&enc_lock[VeChannel]);

        if(VeChannel<DOORDVR_CHANNEL_NUM)
        {
          Hqt_Mpi_PutRecVideoFrame(VeChannel,RECVIDEO_STREAM_ID,&frame_info,VIDEO_IPCAM);/*放入录像缓冲*/
          Hqt_Mpi_PutNetMainVideoFrame(VeChannel%DOORDVR_CHANNEL_NUM,NETMAINVIDEO_STREAM_ID,&frame_info,VIDEO_IPCAM);/*放入网络主码流缓冲*/
          Hqt_Mpi_PutRecVideoFrame(VeChannel,PRERECVIDEO_STREAM_ID,&frame_info,VIDEO_IPCAM);/*放入预录像缓冲*/
        }
        else
        {
          //Hqt_Mpi_PutNetSubVideoFrame(VeChannel%DOORDVR_CHANNEL_NUM,NETSUUBVIDEO_STREAM_ID,&frame_info,VIDEO_IPCAM);/*放入网络主码流缓冲*/
        }
        RK_MPI_MB_ReleaseBuffer(mb);
        pthread_mutex_unlock(&enc_lock[VeChannel]);

        pthread_mutex_lock(DVR_GetEncoderMutexLock(VeChannel, DVR_STREAM_TYPE_H264));
        pthread_cond_broadcast(DVR_GetEncoderCondSignal(VeChannel, DVR_STREAM_TYPE_H264));
        pthread_mutex_unlock(DVR_GetEncoderMutexLock(VeChannel, DVR_STREAM_TYPE_H264));


        /*检测编码器参数*/
/* 检测编码器参数（仅本地通道） */
if (VeChannel < DOORDVR_CHANNEL_NUM && encparam_count >= 5)
{
  MPI_VENC_GetCoderParam(VeChannel, &enc_coder_param); /* 获得正在使用编码器的参数 */
  MPI_VENC_GetEncParam(VeChannel, &enc_param);
  if (memcmp(&enc_coder_param, &enc_param, sizeof(_H264_ENC_PARAM_T) - sizeof(int) * 4))
  {
    MSLOG_DEBUG("encoder params changed, restart encoder on ch %d", VeChannel);
    Hqt_Venc_CloseEncoder(VeChannel);
    Hqt_Venc_InitEncoder(VeChannel);
#ifdef CONFIG_OSD
    // 分辨率/格式变化时重建本地 OSD
    SYS_Osd_Destroy(VeChannel);
    Hqt_Osd_InitRegions(VeChannel);
    SYS_Osd_Create(VeChannel);
#endif
  }
  encparam_count = 0;
}

        encparam_count++;
#ifdef CONFIG_OSD
        // OSD 仅对本地编码通道
        if (VeChannel < DOORDVR_CHANNEL_NUM) {
          if ((osd_endTime > osd_startTime ? (osd_endTime - osd_startTime)
                                           : (osd_startTime - osd_endTime)) > 500000) { // 0.5 s
            osd_startTime = osd_endTime;
            Sys_Osd_Update(VeChannel);
          }
        }
#endif
        //usleep(0);不能sleep会丢帧

      }
    }
  };

//如果  打开在程序退出时会有错误
#ifdef CONFIG_OSD
  if (VeChannel < DOORDVR_CHANNEL_NUM) {
    SYS_Osd_Destroy(VeChannel);
  }
#endif
  free(vpARG);
  vpARG = NULL;
RemoveGetChStream(VeChannel);
	MSLOG_DEBUG("stop h264 stream %d server thread running........",VeChannel);
  pThreadParam->states=STATE_INITIAL;

#endif
  return NULL;
}


