#include "doordvr_ipcam.h"
#include "doordvr_ipcammanage.h"
#include "doordvr_getsetconfig.h"
#include "mslogger.h"
#include "decode_pthread.h"
#include "doordvr_export.h"
#include "ze_rec_read.h"

//#define	IPCAM_RTSP	"rtsp://192.168.0.64:554/Streaming/Channels/101?transportmode=unicast;profile=Profile_1"
//#define IPCAM_RTSP	"rtsp://192.168.0.18:554/1/h264major"
//#define IPCAM_USERNAME	"admin"
//#define IPCAM_PASSWORD	"zhou1979"
//#define IPCAM_PASSWORD	"admin"

CIpCamManage::CIpCamManage()
{
	bzero(m_IpCamConfig, IPCAM_CHANNEL_NUM*sizeof(IPCamEncoderConfig_t));
	for(int i = 0; i < IPCAM_CHANNEL_NUM; i++)
		m_u8NeedIFrame[i] = 1;
}

int CIpCamManage::InitManageIpCam()
{
	int ret = 0;
	//MSLOG_DEBUG("InitManageIpCam!");
	for(int i = 0; i < IPCAM_CHANNEL_NUM; i++)
	{
		DVR_GetConfig(oper_ipcam_config_type, (DVR_U8_T *)(void *)(&m_IpCamConfig[i]), i);
		if(CreateIpCamStream(i) < 0)
		{
			MSLOG_ERROR("CIpCamManage::InitManageIpCam chn(%d) Failed!", i);
			ret = -1;
		}
	}

	return ret;
}

int CIpCamManage::StartManageIpCam()
{
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	mapIpCamIter iter = m_mapIpCamStreamQuery.begin();
	mapIpCamIter iterE = m_mapIpCamStreamQuery.end();
	MSLOG_DEBUG("StartManageIpCam!");
	while (iter != iterE)
	{
		CIpCamStream* pSession = iter->second;
		int chn = pSession->GetChannel() - IPCCHANNELBEGIN;

		StartIpCamStream(pSession, chn);
		++ iter;
		usleep(800 * 1000);
	}
	
	if (m_CheckVideoStreamThread.IsRunning())
	{
		return -1;
	}

	m_CheckVideoStreamThread.StartThread(IpCheckVideoThread, this);

  if(!m_CheckIPCNetworkEnvironmentThread.IsRunning()){
      m_CheckIPCNetworkEnvironmentThread.StartThread(checkIPCNetworkEnvironment,this);
  }

	return 0;
}

int CIpCamManage::StopManageIpCam()
{
	CMsAutoLock lock(m_lockIpCamStreamQuery);
    mapIpCamIter iter = m_mapIpCamStreamQuery.begin();
	mapIpCamIter iterE = m_mapIpCamStreamQuery.end();
	while (iter != iterE)
	{
		CIpCamStream* pSession = (*iter).second;
		pSession->IpCamStop();
		delete pSession;
		pSession = NULL;
		++ iter;
	}
	m_mapIpCamStreamQuery.clear();
	MSLOG_DEBUG("");
	if (!m_CheckVideoStreamThread.IsRunning())
	{
		return -1;
	}
	m_CheckVideoStreamThread.StopThread();
	MSLOG_DEBUG("");
    if(m_CheckIPCNetworkEnvironmentThread.IsRunning()){
        m_CheckIPCNetworkEnvironmentThread.StopThread();
    }

	return 0;
}

bool CIpCamManage::GetVideoLost(uint8_t chn, bool isMain)
{
	bool ret = false;
	int chnStream;
	CMsAutoLock lock(m_lockIpCamStreamQuery);

	if(isMain)
	{
		chnStream = chn;
	}
	else
	{
		chnStream = chn + MAXIPCNUM;
	}
	
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chnStream);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//判断码流类型是否一致
		if((isMain && pSession->GetStreamType() == IPCAM_MAIN_STREAM) ||
			(!isMain && pSession->GetStreamType() == IPCAM_SUB_STREAM))
		{
			ret = pSession->GetVideoLost();
		}
		else
		{
			MSLOG_ERROR("CIpCamManage::GetVideoLost chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
		}		
	} 
	else
	{
		MSLOG_ERROR("CIpCamManage::GetVideoLost chn(%d) Not Finded!", chn);
	}

	return ret;
}

int CIpCamManage::GetChnSize(uint8_t chn, bool isMain, uint32_t *width, uint32_t *height)
{
	int ret = -1;
	int chnStream;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	if(isMain)
	{
		chnStream = chn;
	}
	else
	{
		chnStream = chn + MAXIPCNUM;
	}
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chnStream);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//判断码流类型是否一致
		if((isMain && pSession->GetStreamType() == IPCAM_MAIN_STREAM) ||
			(!isMain && pSession->GetStreamType() == IPCAM_SUB_STREAM))
		{
			ret = pSession->GetPicSize(width, height);
		}
		else
		{
			MSLOG_WARN("CIpCamManage::GetPicSize chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
		}
	} 
	else
	{
		MSLOG_WARN("CIpCamManage::GetPicSize chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
	}

	return ret;
}

//获取通道帧率
int CIpCamManage::GetChnFrameRate(uint8_t chn, bool isMain, uint32_t *frameRate)
{
	int ret = -1;
	int chnStream;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	if(isMain)
	{
		chnStream = chn;
	}
	else
	{
		chnStream = chn + MAXIPCNUM;
	}
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chnStream);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//判断码流类型是否一致
		if((isMain && pSession->GetStreamType() == IPCAM_MAIN_STREAM) ||
			(!isMain && pSession->GetStreamType() == IPCAM_SUB_STREAM))
		{
			ret = pSession->GetFrameRate(frameRate);
		}
		else
		{
			MSLOG_WARN("CIpCamManage::GetChnFrameRate chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
		}
	} 
	else
	{
		MSLOG_WARN("CIpCamManage::GetChnFrameRate chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
	}

	return ret;
}


//获取通道是否带音
int CIpCamManage::GetChnAudioFlag(uint8_t chn, bool isMain, uint32_t *audioFlag)
{
	int ret = -1;
	int chnStream;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	if(isMain)
	{
		chnStream = chn;
	}
	else
	{
		chnStream = chn + MAXIPCNUM;
	}
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chnStream);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//判断码流类型是否一致
		if((isMain && pSession->GetStreamType() == IPCAM_MAIN_STREAM) ||
			(!isMain && pSession->GetStreamType() == IPCAM_SUB_STREAM))
		{
			ret = pSession->GetAudioFlag(audioFlag);
		}
		else
		{
			MSLOG_WARN("CIpCamManage::GetChnAudioFlag chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
		}
	} 
	else
	{
		MSLOG_WARN("CIpCamManage::GetChnAudioFlag chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
	}

	return ret;
}

//获取编码格式
int CIpCamManage::GetEncoderFormat(uint8_t chn, bool isMain)
{
	int ret = -1;
	int chnStream;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	if(isMain)
	{
		chnStream = chn;
	}
	else
	{
		chnStream = chn + MAXIPCNUM;
	}
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chnStream);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//判断码流类型是否一致
		if((isMain && pSession->GetStreamType() == IPCAM_MAIN_STREAM) ||
			(!isMain && pSession->GetStreamType() == IPCAM_SUB_STREAM))
		{
			ret = pSession->GetEncoderType();
		}
		else
		{
			MSLOG_WARN("CIpCamManage::GetEncoderFormat chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
		}
	} 
	else
	{
		MSLOG_WARN("CIpCamManage::GetEncoderFormat chn(%d),type(%s) Not Finded!", chn, isMain?"main":"sub");
	}

	return ret;

}

int CIpCamManage::SetIpCamAudioStreamCallback(uint8_t chn, EncoderDataCallback cbfunc, void *pUsrData)
{
	int ret = -1;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
  mapIpCamIter iter = m_mapIpCamStreamQuery.begin();
	mapIpCamIter iterE = m_mapIpCamStreamQuery.end();
	while (iter != iterE)
	{
		CIpCamStream* pSession = (*iter).second;
		if(chn == (*iter).first)
		{
			pSession->SetIpCamAudioStreamCallback(cbfunc, pUsrData);
			ret = 0;
		}
		else
		{
			pSession->ClearIpCamAudioStreamCallback();
		}
		++ iter;
	}

	return ret;
}

int CIpCamManage::ClearIpCamAudioStreamCallback()
{
	CMsAutoLock lock(m_lockIpCamStreamQuery);
  mapIpCamIter iter = m_mapIpCamStreamQuery.begin();
	mapIpCamIter iterE = m_mapIpCamStreamQuery.end();
	while (iter != iterE)
	{
		CIpCamStream* pSession = (*iter).second;
		pSession->ClearIpCamAudioStreamCallback();
		++ iter;
	}

	return 0;
}

int CIpCamManage::ClearAllIpCamVideoStreamCallback()
{
	CMsAutoLock lock(m_lockIpCamStreamQuery);
  mapIpCamIter iter = m_mapIpCamStreamQuery.begin();
	mapIpCamIter iterE = m_mapIpCamStreamQuery.end();
	while (iter != iterE)
	{
		CIpCamStream* pSession = (*iter).second;
		pSession->ClearIpCamVideoStreamCallback();
		++ iter;
	}

	return 0;
}
void* CIpCamManage::checkIPCNetworkEnvironment(void *pUsrData)
{
    CIpCamManage* pThis = (CIpCamManage*)pUsrData;
    pThis->checkIPCNetworkEnvironmentProcess();
    return 0;
}
void    CIpCamManage::checkIPCNetworkEnvironmentProcess()
{
    MSLOG_DEBUG("start checkIPCNetworkEnvironment thread running........thread pid = %u",(unsigned int)syscall(224));
    while(!m_CheckIPCNetworkEnvironmentThread.GetExit()){
        int inet_sock;
        struct ifreq ifr;


        inet_sock = socket(AF_INET, SOCK_DGRAM, 0);

        if(inet_sock <0){
            MSLOG_DEBUG("create socket error \n");
            usleep(5000 * 1000);
            continue;
        }
        strcpy(ifr.ifr_name, "eth0");
        if(ioctl(inet_sock, SIOCGIFFLAGS, &ifr) < 0)
        {
            close(inet_sock);
            MSLOG_DEBUG("ioctl socket error \n");
            usleep(5000 * 1000);
            continue;
        }

        if(ifr.ifr_ifru.ifru_flags &IFF_UP)
        {
            if (ioctl(inet_sock, SIOCGIFADDR, &ifr)<0){
                MSLOG_DEBUG("eth0 is up ,but don't have ip , reinit...");
                NetworkParam_t netparam;
                DVR_GetConfig(oper_networkparm_type, (DVR_U8_T *)(&netparam), 0);
                NetWorkConfig_Init(netparam);

            }else{
                MSLOG_DEBUG("etho up, ip is: %s \n ", inet_ntoa(((struct sockaddr_in*)&(ifr.ifr_addr))->sin_addr));
                m_CheckIPCNetworkEnvironmentThread.SetExit();
                system("route add -net 239.255.255.0/24 eth0");
            }

        }
        else
        {
            MSLOG_DEBUG("eth0 is down, reinit...");
            ifr.ifr_ifru.ifru_flags = ifr.ifr_ifru.ifru_flags|IFF_UP;

            if(ioctl(inet_sock, SIOCSIFFLAGS, &ifr)<0){
                MSLOG_DEBUG("set eth0 up failed");
            }
            usleep(2000 * 1000);
            NetworkParam_t netparam;
            DVR_GetConfig(oper_networkparm_type, (DVR_U8_T *)(&netparam), 0);
            NetWorkConfig_Init(netparam);
        }
        close(inet_sock);

        usleep(1000 * 1000);
    }
		
    MSLOG_DEBUG("stop checkIPCNetworkEnvironment thread running........thread pid = %u",(unsigned int)syscall(224));
}

/***********************************************************************************************************
**函数:IpCheckVideoThread
**输入参数:
**功能: 1）检查实时预览是否结束，清屏
**      2）检查视频丢失
**      3）间隔启动IPCAM      
**返回值:
***********************************************************************************************************/
void* CIpCamManage::IpCheckVideoThread(void *pUsrData)
{
	CIpCamManage* pThis = (CIpCamManage*)pUsrData;
	pThis->IpCheckVideoProcess();
	return 0;
}

void 	CIpCamManage::IpCheckVideoProcess()
{
	int timecount = 0;
	MSLOG_DEBUG("start IpCheckVideoStream thread running........thread pid = %u",(unsigned int)syscall(224));
	while(!m_CheckVideoStreamThread.GetExit())
	{
		long long curtime = local_get_curtime();
		for(int camChn = 0; camChn < MAXIPCNUM; camChn++)
		{
			if(m_updatetime[camChn])
			{
				//MSLOG_DEBUG("Check Chn %d, ScreenSplit %d,sizeof(%d)", camChn, GetCurrentChannel(), sizeof(GetCurrentChannel()));
				//每500ms检测一次是否有通道结束播放
			  if((curtime > m_updatetime[camChn] ? curtime - m_updatetime[camChn]: m_updatetime[camChn] - curtime) > 500 * 1000)
			  {
					switch(GetCurrentChannel())
		      {
						case L_CH9_FULL_SCREEN:	
							//MSLOG_DEBUG("Clear!!");
							if(camChn == 0)Hqt_Vivo_ClearChnVmix(0);
							break;
						case L_CH10_FULL_SCREEN:
						case L_CH11_FULL_SCREEN:
						case L_CH12_FULL_SCREEN:
						case L_CH13_FULL_SCREEN:
						case L_CH14_FULL_SCREEN:
						case L_CH15_FULL_SCREEN:
						case L_CH16_FULL_SCREEN:
							//MSLOG_DEBUG("Clear!!!!");
							if(camChn == GetCurrentChannel() - L_CH10_FULL_SCREEN + 1) Hqt_Vivo_ClearChnVmix(0);
		          break;
		        case L_NINE_PICTURE:
							//MSLOG_DEBUG("Clear!!!");
		          Hqt_Vivo_ClearChnVmix(8);
		          break;
		      }
					//清除9分割
					if((GetCurrentChannel() & 0x800000FF) == 0x800000FF)Hqt_Vivo_ClearChnVmix(8);
					m_updatetime[camChn] = 0;
		    }
			  
			}
		}
		if(timecount++ > 9)
		{
			//每500ms*10检查一次连接状态
			for(int camChn = 0; camChn < MAXIPCNUM; camChn++)
			{
				//MSLOG_DEBUG("chn(%d) Enable(%d) videolost(%d)", camChn, m_IpCamConfig[camChn].Enable, GetVideoLost(camChn + 9));
				if(m_IpCamConfig[camChn].Enable && GetVideoLost(camChn + 9, true))
				{
					uint8_t result = 1;
					MessageQueue_Send_Process(MSP_CMD_UPDATE_IPADDRESS, (DVR_U8_T *)&result, sizeof(DVR_U8_T), SEND_MESSAGE_TO_UI);
					//MSLOG_DEBUG("Send MSP_CMD_UPDATE_IPADDRESS!");
					break;
				}
			}
			timecount = 0;
		}
		usleep(500 * 1000);
	}
	
	MSLOG_DEBUG("stop IpCheckVideoStream thread running........thread pid = %u",(unsigned int)syscall(224));
}

int CIpCamManage::SetIpCamVideoStreamCallback(uint8_t chn, EncoderDataCallback cbfunc, void *pUsrData)
{
	int ret = -1;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chn);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		ret = pSession->SetIpCamVideoStreamCallback(cbfunc, pUsrData);
	} 
	else
	{
		MSLOG_ERROR("CIpCamManage::SetIpCamVideoStreamCallback chn(%d) Not Finded!", chn);
	}

	return ret;
}

int CIpCamManage::IpcEncoderParamCallback(int chn, IPCAMSTREAM stream, IPC_ENCODER_PARAM_T *stIpcParam, void *pUsrData)
{
	UPDATE_IPC_ENCODER_PARAM param;
	param.chn = chn - IPCCHANNELBEGIN;
	param.stream = stream;
	param.encoder = stIpcParam->uEncoderType;
	//MSLOG_DEBUG("SEND MSP_CMD_IPCAM_UPDATE_ENCODER_PARAM:%d %d %d",param.chn, param.stream, param.encoder);
	MessageQueue_Send_Process(MSP_CMD_IPCAM_UPDATE_ENCODER_PARAM, (uint8_t*)&param, sizeof(UPDATE_IPC_ENCODER_PARAM), SEND_MESSAGE_TO_UI); //通知编码器参数更新
	return 0;
}


bool CIpCamManage::IsValid(uint8_t chn, bool isMain)
{
	bool ret = false;
	int chnStream;
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	
	if(isMain)
	{
		chnStream = chn;
	}
	else
	{
		chnStream = chn + MAXIPCNUM;
	}
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(chnStream);
	
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//判断码流类型是否一致
		if((isMain && pSession->GetStreamType() == IPCAM_MAIN_STREAM) ||
			(!isMain && pSession->GetStreamType() == IPCAM_SUB_STREAM))
		{
			ret = true;
		}
	}

	return ret;
}

int CIpCamManage::IpCamCallback(int chn, int stream_type, FRAME_INFO_T *frame_buf, void *pUsrData)
{
	CIpCamManage* pThis = (CIpCamManage*)pUsrData;

	return pThis->IpCamCb(chn, stream_type, frame_buf);
}

int CIpCamManage::IpCamCb(int chn, int stream_type, FRAME_INFO_T *frame_buf)
{
	int decChn = GetVDecChn(chn);
	int IpCamChn = chn - IPCCHANNELBEGIN;
	DVR_I32_T ret = 0;
	//DVR_DEBUG("IpCamCb chn%d,%d",chn, decChn);
	//子码流通道号处理
	IpCamChn %= MAXIPCNUM;
	if (decChn >= 0 && IpCamChn < MAXIPCNUM)
	{
		//解码器通道号正常
		//mdv_playback_cb(decChn, stream_type, frame_buf->frameType, frame_buf->length, frame_buf->relativeTime, 
		//	(char*)frame_buf->pData, &status);
		
		CMsAutoLock lock(m_lockIpCamPreviewBuffer[IpCamChn]);
		if(m_u8NeedIFrame[IpCamChn])
		{
			if(!frame_buf->keyFrame) return 0;
			m_u8NeedIFrame[IpCamChn] = 0;
		}
		switch(frame_buf->frameType)
	  {
	    case FRAME_TYPE_H264I:
	    case FRAME_TYPE_H264P:
	    {
	      m_PreviewBuffer[IpCamChn] = RK_MPI_MB_CreateBuffer(frame_buf->length, RK_FALSE, 0);
	      memcpy(RK_MPI_MB_GetPtr(m_PreviewBuffer[IpCamChn]), frame_buf->pData, frame_buf->length);
	      RK_MPI_MB_SetSize(m_PreviewBuffer[IpCamChn], frame_buf->length);
				//MSLOG_DEBUG("Send %d to %d", frame_buf->length, decChn);
	      ret = Hqt_Dec_VdecEndStream(decChn, m_PreviewBuffer[IpCamChn], frame_buf->length, false);
	      if(ret < 0)
	      {
	        MSLOG_ERROR("put Chn%d to VdecChn%d Len(%d)failed...\n", IpCamChn, decChn, frame_buf->length);
	      }
#if 0				
	      MSLOG_DEBUG("send chn[%d]framelen(%d):%d,%d,%d,%d,%d,%d,%d,%d", chn, frame_buf->length, frame_buf->pData[0],
	      frame_buf->pData[1],frame_buf->pData[2],frame_buf->pData[3],frame_buf->pData[4],
	      frame_buf->pData[5],frame_buf->pData[6],frame_buf->pData[7]);
#endif				
	      RK_MPI_MB_ReleaseBuffer(m_PreviewBuffer[IpCamChn]);
	      m_PreviewBuffer[IpCamChn] = NULL;
				m_updatetime[IpCamChn] = local_get_curtime();
	    }
	    break;
	  }
	}
	return 0;
}

/***********************************************************************************************************
**函数:
**输入参数:chn_num 通道chn的个数，IPCAM通道号int32_t* chn
**功能:IPCAM码流绑定VDEC,如果chn_num==1则使用主码流，其他优先使用子码流，子码流没有再使用主码流
**返回值:成功--0，失败---1
***********************************************************************************************************/
int CIpCamManage::BindVdec(int32_t chn_num, int32_t* chn)
{
	int vDecChn = 0;
	char codec[2][5]= {"h264","h265"};
	REC_FILEINFO_T avi_info;
	//MSLOG_DEBUG("BindVdec!");
	ClearAllIpCamVideoStreamCallback();
	m_mapIpcVdec.clear();
	for(int i = 0; i < chn_num; i++)
	{
		if(Hqt_GetVDecEnable(vDecChn))Hqt_Dec_StopH264Vdec(vDecChn);
		if(chn_num == 1)
		{
			//使用主码流
			if(IsValid(chn[i], true))
			{
				//创建VDEC通道
				int chncodec = GetEncoderFormat(chn[i], true);
				if(chncodec == 1)
					avi_info.compressor = codec[1];
				else
					avi_info.compressor = codec[0];
				Hqt_Dec_CreateChn(vDecChn, &avi_info);
				m_u8NeedIFrame[i] = 1;
				SetIpCamVideoStreamCallback(chn[i], IpCamCallback, this);
				m_mapIpcVdec.insert(mapIpCamVDec::value_type(chn[i],vDecChn));
				MSLOG_DEBUG("chn[%d] %s bind VDecChn%d",chn[i],avi_info.compressor,vDecChn);
				vDecChn++;
			}
		}
		else
		{
			//使用子码流
			if(IsValid(chn[i], false))
			{
				//创建VDEC通道
				int chncodec = GetEncoderFormat(chn[i], false);
				if(chncodec == 1)
					avi_info.compressor = codec[1];
				else
					avi_info.compressor = codec[0];
				Hqt_Dec_CreateChn(vDecChn, &avi_info);
				m_u8NeedIFrame[i] = 1;
				SetIpCamVideoStreamCallback(chn[i] + MAXIPCNUM, IpCamCallback, this);
				m_mapIpcVdec.insert(mapIpCamVDec::value_type(chn[i], vDecChn));
				MSLOG_DEBUG("chn[%d] %s bind VDecChn%d",chn[i] + MAXIPCNUM,avi_info.compressor,vDecChn);
				vDecChn++;
			}
			else
			{
				//使用主码流
				if(IsValid(chn[i], true))
				{
					//创建VDEC通道
					int chncodec = GetEncoderFormat(chn[i], false);
					if(chncodec == 1)
						avi_info.compressor = codec[1];
					else
						avi_info.compressor = codec[0];
					Hqt_Dec_CreateChn(vDecChn, &avi_info);
					m_u8NeedIFrame[i] = 1;
					MSLOG_DEBUG("chn[%d] %s bind VDecChn%d",chn[i] + MAXIPCNUM,avi_info.compressor,vDecChn);
					SetIpCamVideoStreamCallback(chn[i], IpCamCallback, this);
					m_mapIpcVdec.insert(mapIpCamVDec::value_type(chn[i],vDecChn));
					vDecChn++;
				}
			}
		}
	}
	
	return 0;
}

/***********************************************************************************************************
**函数:
**输入参数:IPCAM通道号 chn,
**功能:获得IPCAM通道chn所绑定的VDEC通道号
**返回值:成功--通道号，失败---1
***********************************************************************************************************/
int CIpCamManage::GetVDecChn(int chn)
{
    int decChn = -1;
    // 原来是 >
    if (chn >= IPCCHANNELBEGIN + MAXIPCNUM) chn -= MAXIPCNUM;  // 归一化到主/子共用键
    mapIpCamVDecIter iter = m_mapIpcVdec.find(chn);
    if (iter != m_mapIpcVdec.end()) decChn = iter->second;
    return decChn;
}


/***********************************************************************************************************
**函数:SetChnConfig
**输入参数:IPCAM参数 config，IPCAM通道号 chn
**功能:设置通道参数配置，chn==IPCAM_CHANNEL_NUM 全部通道参数
**返回值:成功-- 0，失败-- -1
***********************************************************************************************************/
int CIpCamManage::SetChnConfig(IPCamEncoderConfig_t* config, uint32_t chn)
{
	int ret = 0;
	if(chn == IPCAM_CHANNEL_NUM)
	{
		for(int i = 0; i < IPCAM_CHANNEL_NUM; i++)
		{
			if(ChangeChnConfig(&config[i], i) < 0)
				ret = -1;
		}
	}
	else
	{
		ret = ChangeChnConfig(config, chn);
	}

	return ret;
}

/***********************************************************************************************************
**函数:ChangeChnConfig
**输入参数:IPCAM参数 config，IPCAM通道号 chn从0开始的通道号
**功能:修改通道参数配置
**返回值:成功-- 0，失败-- -1
***********************************************************************************************************/
int CIpCamManage::ChangeChnConfig(IPCamEncoderConfig_t* config, uint32_t chn)
{
	int ret = 0;

	if(chn >= IPCAM_CHANNEL_NUM) return -1;
	if(m_IpCamConfig[chn].Enable == 1 && config->Enable == 0)
	{
		//停止通道
		MSLOG_DEBUG("Stop chn%d",chn);
		ret = DestroyIpCamStream(chn);
		m_IpCamConfig[chn] = *config;
	}
	else if(m_IpCamConfig[chn].Enable == 0 && config->Enable == 1)
	{
		//开启通道
		MSLOG_DEBUG("Start chn%d",chn);
		m_IpCamConfig[chn] = *config;
		ret = CreateIpCamStream(chn);
		if(!ret)
		{
			ret = StartIpCamStream(chn);
		}
	}
	else if(m_IpCamConfig[chn].Enable == 1 && config->Enable == 1)
	{
		//通道复位
		if(memcmp(&m_IpCamConfig[chn], config, sizeof(IPCamEncoderConfig_t)))
		{
			MSLOG_DEBUG("ReStart chn%d",chn);
	/*				
			char* str = (char*)&m_IpCamConfig[chn];
			printf("OldConfig:");
			for(int num = 0;num < sizeof(IPCamEncoderConfig_t); num++)
				printf("0x%x ",str[num]);
			printf("\r\n");
			str = (char*)config;
			printf("NewConfig:");
			for(int num = 0;num < sizeof(IPCamEncoderConfig_t); num++)
				printf("0x%x ",str[num]);
			printf("\r\n");
	
			MSLOG_DEBUG("OldConfig %d,%d,%s,%d,%s,%s,%s,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%s",
				m_IpCamConfig[chn].Enable,m_IpCamConfig[chn].Channel,m_IpCamConfig[chn].IPAddress,
				m_IpCamConfig[chn].PrefixLength,m_IpCamConfig[chn].UserName,m_IpCamConfig[chn].PassWord,
				m_IpCamConfig[chn].MainStream,m_IpCamConfig[chn].MainStreamWidth,m_IpCamConfig[chn].MainStreamHeight,
				m_IpCamConfig[chn].MainStreamFrameRate,m_IpCamConfig[chn].MainStreamAudioFlag,
				m_IpCamConfig[chn].SubStream,m_IpCamConfig[chn].SubStreamWidth,m_IpCamConfig[chn].SubStreamHeight,
				m_IpCamConfig[chn].SubStreamFrameRate,m_IpCamConfig[chn].SubStreamAudioFlag,
				m_IpCamConfig[chn].Encoding,m_IpCamConfig[chn].UUID);
			MSLOG_DEBUG("NewConfig %d,%d,%s,%d,%s,%s,%s,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%s",
				config->Enable,config->Channel,config->IPAddress,
				config->PrefixLength,config->UserName,config->PassWord,
				config->MainStream,config->MainStreamWidth,config->MainStreamHeight,
				config->MainStreamFrameRate,config->MainStreamAudioFlag,
				config->SubStream,config->SubStreamWidth,config->SubStreamHeight,
				config->SubStreamFrameRate,config->SubStreamAudioFlag,
				config->Encoding,config->UUID);
	*/			
			m_IpCamConfig[chn] = *config;
			DestroyIpCamStream(chn);
			ret = CreateIpCamStream(chn);
			if(!ret)
			{
				ret = StartIpCamStream(chn);
			}
		}
	}
		
	return ret;
}


/***********************************************************************************************************
**函数:CreateIpCamStream
**输入参数:IPCAM通道号 chn 从0开始的通道号
**功能:按照参数配置实例化ipcam
**返回值:成功-- 0，失败-- -1
***********************************************************************************************************/
int CIpCamManage::CreateIpCamStream(uint32_t chn)
{
	if(chn >= IPCAM_CHANNEL_NUM) return -1;
	MSLOG_DEBUG("CreateIpCamStream chn%d",chn);
	if(m_IpCamConfig[chn].Enable && m_IpCamConfig[chn].MainStream[0] && 
		m_IpCamConfig[chn].MainStreamWidth && m_IpCamConfig[chn].MainStreamHeight)
	{
		CIpCamStream* stream = new CIpCamStream(m_IpCamConfig[chn].MainStream, m_IpCamConfig[chn].Channel,
			IPCAM_MAIN_STREAM, m_IpCamConfig[chn].MainStreamAudioFlag, m_IpCamConfig[chn].MainStreamWidth, 
			m_IpCamConfig[chn].MainStreamHeight, m_IpCamConfig[chn].MainStreamFrameRate, m_IpCamConfig[chn].MainStreamEncoding);
		CMsAutoLock lock(m_lockIpCamStreamQuery);
		m_mapIpCamStreamQuery.insert(mapIpCamStream::value_type(m_IpCamConfig[chn].Channel, stream));
		MSLOG_DEBUG("Create %d:%s",m_IpCamConfig[chn].Channel, m_IpCamConfig[chn].MainStream);
	}
#if 1		
	if(m_IpCamConfig[chn].Enable && m_IpCamConfig[chn].SubStream[0] && m_IpCamConfig[chn].SubStreamWidth && 
		m_IpCamConfig[chn].SubStreamHeight)
	{
		CIpCamStream* stream = new CIpCamStream(m_IpCamConfig[chn].SubStream, m_IpCamConfig[chn].Channel,
			IPCAM_SUB_STREAM, m_IpCamConfig[chn].SubStreamAudioFlag, m_IpCamConfig[chn].SubStreamWidth, 
			m_IpCamConfig[chn].SubStreamHeight, m_IpCamConfig[chn].SubStreamFrameRate, m_IpCamConfig[chn].SubStreamEncoding);
			/*
		CIpCamStream* stream = new CIpCamStream(m_IpCamConfig[i].SubStream, m_IpCamConfig[i].Channel,
			IPCAM_SUB_STREAM, m_IpCamConfig[i].SubStreamAudioFlag, 352, 
			288);	*/
		CMsAutoLock lock(m_lockIpCamStreamQuery);
		m_mapIpCamStreamQuery.insert(mapIpCamStream::value_type(m_IpCamConfig[chn].Channel + MAXIPCNUM, stream));
	}	
#endif	
	return 0;
}

/***********************************************************************************************************
**函数:DestroyIpCamStream
**输入参数:IPCAM通道号 chn从0开始的通道号
**功能:销毁chn通道ipcam
**返回值:成功-- 0，失败-- -1
***********************************************************************************************************/
int CIpCamManage::DestroyIpCamStream(uint32_t chn)
{
	int ret = 0;
	int IpCamChn = chn + IPCCHANNELBEGIN;
	
	if(chn >= IPCAM_CHANNEL_NUM) return -1;
	MSLOG_DEBUG("DestroyIpCamStream chn%",chn);
	CMsAutoLock lock(m_lockIpCamStreamQuery);
	//销毁主码流
	mapIpCamIter iter = m_mapIpCamStreamQuery.find(IpCamChn);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		//MSLOG_DEBUG("DestroyIpCamStream 1");
		pSession->IpCamStop();
		//MSLOG_DEBUG("DestroyIpCamStream 11");
		delete pSession;
		m_mapIpCamStreamQuery.erase(iter);
	} 
	else
	{
		MSLOG_ERROR("CIpCamManage::DestroyIpCamStream chn(%d) Not Finded!", IpCamChn);
		ret = -1;
	}
	//MSLOG_DEBUG("DestroyIpCamStream 111");
	//销毁子码流
	iter = m_mapIpCamStreamQuery.find(IpCamChn + MAXIPCNUM);
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = (*iter).second;
		pSession->IpCamStop();
		delete pSession;
		m_mapIpCamStreamQuery.erase(iter);
	} 
	else
	{
		MSLOG_WARN("CIpCamManage::DestroyIpCamStream chn(%d) Not Finded!", IpCamChn + MAXIPCNUM);
	}
	//MSLOG_DEBUG("DestroyIpCamStream 1111");
	return ret;
}

/***********************************************************************************************************
**函数:DestroyIpCamStream
**输入参数:IPCAM通道号 chn从0开始的通道号
**功能:销毁chn通道ipcam
**返回值:成功-- 0，失败-- -1
***********************************************************************************************************/
int CIpCamManage::StartIpCamStream(uint32_t chn)
{
	int ret = -1;
	int IpCamChn = chn + IPCCHANNELBEGIN;

	if(chn >= IPCAM_CHANNEL_NUM) return -1;
	
	CMsAutoLock lock(m_lockIpCamStreamQuery);

	mapIpCamIter iter = m_mapIpCamStreamQuery.find(IpCamChn);
	
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = iter->second;
		ret = StartIpCamStream(pSession, chn);
	}

	//开始子码流
	iter = m_mapIpCamStreamQuery.find(IpCamChn + MAXIPCNUM);
	
	if (iter != m_mapIpCamStreamQuery.end())
	{
		CIpCamStream* pSession = iter->second;
		ret = StartIpCamStream(pSession, chn);
	}
	return ret;
}

int CIpCamManage::StartIpCamStream(CIpCamStream* pSession, uint32_t chn)
{
	//MSLOG_DEBUG("");
	string name = m_IpCamConfig[chn].UserName;
	string password = m_IpCamConfig[chn].PassWord;

	pSession->SetUserNamePwd(name, password);
#if 0	
	if(m_IpCamConfig[chn].UserName[0])
	{
		string name = m_IpCamConfig[chn].UserName;
		pSession->SetUserName(name);
	}
	MSLOG_DEBUG("");
	if(m_IpCamConfig[chn].PassWord[0])
	{
		string password = m_IpCamConfig[chn].PassWord;
		pSession->SetUserPwd(password);
	}
	MSLOG_DEBUG("");
#endif
	pSession->SetIpCamEncoderParamCallback(IpcEncoderParamCallback, this);
	pSession->IpCamStart();
	
	return 0;
}

bool CIpCamManage::WaitVdecReadyIdx_(int idx, int wait_ms) {
  if (idx < 0 || idx >= MAXIPCNUM * 2) return false; // 主+子各 MAXIPCNUM
  long long start_us = local_get_curtime();
  long long deadline = start_us + (long long)wait_ms * 1000;
  long long first    = m_updatetime[idx]; // 进入前的时间戳

  while (local_get_curtime() < deadline) {
    if (m_updatetime[idx] != first) return true; // 收到新帧
    usleep(50 * 1000); // 50ms
  }
  return false;
}


int CIpCamManage::SnapshotJpeg(uint8_t chn, bool isMain, const char* jpg_path,
                               int timeout_ms, int quality) {
  if (!jpg_path || !jpg_path[0]) {
    MSLOG_ERROR("SnapshotJpeg: invalid jpg_path");
    return -1;
  }

  // 绝对通道：9,10,...   内部索引：主用 chn，子用 chn+MAXIPCNUM
  const int ipcamAbs = IPCCHANNELBEGIN + (isMain ? chn : (chn + MAXIPCNUM));
  const int idx      = isMain ? chn : (chn + MAXIPCNUM);

  // 1) 找到 VDEC
  const int vdecChn = GetVDecChn(ipcamAbs);
  if (vdecChn < 0) {
    MSLOG_ERROR("SnapshotJpeg: no VDEC bound for ipcam_abs=%d (idx=%d isMain=%d)",
                ipcamAbs, chn, (int)isMain);
    return -1;
  }

  // 2) 分辨率
  uint32_t w = 0, h = 0;
  if (GetChnSize(ipcamAbs, isMain, &w, &h) < 0 || w == 0 || h == 0) {
    MSLOG_ERROR("SnapshotJpeg: GetChnSize failed (absChn=%d isMain=%d)",
                ipcamAbs, (int)isMain);
    return -1;
  }
  const uint32_t vw = (w + 15) & ~15;
  const uint32_t vh = (h + 15) & ~15;

  // 让 VDEC 先真的“出一帧”
  if (!WaitVdecReadyIdx_(idx, 1500)) {
    MSLOG_WARN("SnapshotJpeg: VDEC(%d) not ready, wait a bit...", vdecChn);
    usleep(300 * 1000); // 再保守等 300ms
  }

  // VENC/JPEG 基础参数
  VENC_CHN_ATTR_S veAttr; memset(&veAttr, 0, sizeof(veAttr));
  veAttr.stVencAttr.enType       = RK_CODEC_TYPE_JPEG;
  veAttr.stVencAttr.imageType    = IMAGE_TYPE_NV12; // RGA 输出线性 NV12
  veAttr.stVencAttr.u32PicWidth  = w;
  veAttr.stVencAttr.u32PicHeight = h;
  veAttr.stVencAttr.u32VirWidth  = vw;
  veAttr.stVencAttr.u32VirHeight = vh;

  auto capture_once = [&](const char* path)->int {
    // 3) 创建 VENC
    VENC_CHN vencChn = -1;
    for (int ch = VENC_MAX_CHN_NUM - 1; ch >= 0; --ch) {
      if (RK_MPI_VENC_CreateChn(ch, &veAttr) == 0) { vencChn = ch; break; }
      if (RK_MPI_VENC_CreateJpegLightChn(ch, &veAttr) == 0) { vencChn = ch; break; }
    }
    if (vencChn < 0) { MSLOG_ERROR("SnapshotJpeg: create VENC failed"); return -1; }

    VENC_JPEG_PARAM_S jpgParam; memset(&jpgParam, 0, sizeof(jpgParam));
    int q = (quality < 1) ? 1 : (quality > 100 ? 100 : quality);
    jpgParam.u32Qfactor = q;
    RK_MPI_VENC_SetJpegParam(vencChn, &jpgParam);
    RK_MPI_SYS_SetMediaBufferDepth(RK_ID_VENC, vencChn, 2);

    // （可选）给 VDEC 多留几帧缓存，避免瞬时空
    RK_MPI_SYS_SetMediaBufferDepth(RK_ID_VDEC, vdecChn, 4);

    // 4) 创建 RGA（不启内部池）
    RGA_CHN rgaChn = -1;
    RGA_ATTR_S rgaAttr; memset(&rgaAttr, 0, sizeof(rgaAttr));
    rgaAttr.bEnBufPool = RK_FALSE;
    rgaAttr.u16Rotaion = 0;

    rgaAttr.stImgIn.imgType       = IMAGE_TYPE_NV12;
    rgaAttr.stImgIn.u32Width      = w;
    rgaAttr.stImgIn.u32Height     = h;
    rgaAttr.stImgIn.u32HorStride  = vw;
    rgaAttr.stImgIn.u32VirStride  = vh;

    rgaAttr.stImgOut.imgType      = IMAGE_TYPE_NV12;
    rgaAttr.stImgOut.u32Width     = w;
    rgaAttr.stImgOut.u32Height    = h;
    rgaAttr.stImgOut.u32HorStride = vw;
    rgaAttr.stImgOut.u32VirStride = vh;

    for (int ch = 0; ch < RGA_MAX_CHN_NUM; ++ch) {
      if (RK_MPI_RGA_CreateChn(ch, &rgaAttr) == 0) { rgaChn = ch; break; }
    }
    if (rgaChn < 0) {
      MSLOG_ERROR("SnapshotJpeg: create RGA chn failed");
      RK_MPI_VENC_DestroyChn(vencChn);
      return -1;
    }

    // 5) 绑定：VDEC -> RGA -> VENC
    MPP_CHN_S src{}, mid{}, dst{};
    src.enModId = RK_ID_VDEC; src.s32ChnId = vdecChn;
    mid.enModId = RK_ID_RGA;  mid.s32ChnId = rgaChn;
    dst.enModId = RK_ID_VENC; dst.s32ChnId = vencChn;

    if (RK_MPI_SYS_Bind(&src, &mid) != 0) {
      MSLOG_ERROR("SnapshotJpeg: Bind VDEC(%d)->RGA(%d) failed", vdecChn, rgaChn);
      RK_MPI_RGA_DestroyChn(rgaChn);
      RK_MPI_VENC_DestroyChn(vencChn);
      return -1;
    }
    if (RK_MPI_SYS_Bind(&mid, &dst) != 0) {
      MSLOG_ERROR("SnapshotJpeg: Bind RGA(%d)->VENC(%d) failed", rgaChn, vencChn);
      RK_MPI_SYS_UnBind(&src, &mid);
      RK_MPI_RGA_DestroyChn(rgaChn);
      RK_MPI_VENC_DestroyChn(vencChn);
      return -1;
    }

    // 给链路一点时间（RGA / VDEC 对齐）
    usleep(120 * 1000);

    // 6) 收 1 帧 JPEG
    VENC_RECV_PIC_PARAM_S recvParam; recvParam.s32RecvPicNum = 1;
    RK_MPI_VENC_StartRecvFrame(vencChn, &recvParam);
    RK_MPI_SYS_StartGetMediaBuffer(RK_ID_VENC, vencChn);

    MEDIA_BUFFER out = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, vencChn, timeout_ms);
    if (!out) {
      MSLOG_ERROR("SnapshotJpeg: get JPEG stream timeout after %d ms", timeout_ms);
      RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, vencChn);
      RK_MPI_SYS_UnBind(&mid, &dst);
      RK_MPI_SYS_UnBind(&src, &mid);
      RK_MPI_RGA_DestroyChn(rgaChn);
      RK_MPI_VENC_DestroyChn(vencChn);
      return -1;
    }

    int fd = ::open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
      MSLOG_ERROR("SnapshotJpeg: open('%s') failed", path);
      RK_MPI_MB_ReleaseBuffer(out);
      RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, vencChn);
      RK_MPI_SYS_UnBind(&mid, &dst);
      RK_MPI_SYS_UnBind(&src, &mid);
      RK_MPI_RGA_DestroyChn(rgaChn);
      RK_MPI_VENC_DestroyChn(vencChn);
      return -1;
    }

    void*  ptr  = RK_MPI_MB_GetPtr(out);
    size_t size = RK_MPI_MB_GetSize(out);
    ssize_t wr  = ::write(fd, ptr, size);
    ::close(fd);

    RK_MPI_MB_ReleaseBuffer(out);
    RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, vencChn);

    // 解绑 & 销毁
    RK_MPI_SYS_UnBind(&mid, &dst);
    RK_MPI_SYS_UnBind(&src, &mid);
    RK_MPI_RGA_DestroyChn(rgaChn);
    RK_MPI_VENC_DestroyChn(vencChn);

    if (wr < 0 || (size_t)wr != size) {
      MSLOG_ERROR("SnapshotJpeg: write('%s') failed (%zd/%zu)", path, wr, size);
      return -1;
    }

    MSLOG_INFO("SnapshotJpeg OK: absChn=%d(idx=%d) isMain=%d vdec=%d => %s (%ux%u q=%d)",
               ipcamAbs, (int)chn, (int)isMain, vdecChn, path, w, h, q);
    return 0;
  };

  // 一次不行就再来一次（多数是就绪边界问题）
  int rc = capture_once(jpg_path);
  if (rc != 0) {
    MSLOG_WARN("SnapshotJpeg: first try failed, retrying...");
    // 再等一下新帧
    WaitVdecReadyIdx_(idx, 800);
    usleep(120 * 1000);
    rc = capture_once(jpg_path);
  }
  return rc;
}

通过以下接口调用
auto* mgr = CIpCamManage::Instance();
int absCh = 10;                       // 绑定 10 的主码流
mgr->BindVdec(1, &absCh);
usleep(200 * 1000);

int ret = mgr->SnapshotJpeg(10 - IPCCHANNELBEGIN, true,
                            "/tmp/ipc10_main.jpg", 5000, 90);
//           ^^^^^^^^^^^^^^^  传入内部 idx（0基）：10-IPCCHANNELBEGIN


