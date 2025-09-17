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


























// --- 通道号归一：返回 9-based，非法返回 -1 ---
int CIpCamManage::NormalizeAbsCh_(int ch) const {
  if (ch >= IPCCHANNELBEGIN && ch < IPCCHANNELBEGIN + MAXIPCNUM) return ch;     // 9..16
  if (ch >= 0 && ch < MAXIPCNUM) return IPCCHANNELBEGIN + ch;                   // 0..7 -> 9..16
  return -1;
}

// --- 打印 VDEC 绑定表 ---
void CIpCamManage::DumpVdecMap_() const {
  char buf[64];
  std::string s = "VDEC MAP: { ";
  for (auto &kv : m_mapIpcVdec) {
    std::snprintf(buf, sizeof(buf), "%d->%d ", (int)kv.first, (int)kv.second);
    s += buf;
  }
  s += "}";
  MSLOG_INFO("%s", s.c_str());
}

int CIpCamManage::EnsureVdecForAbsCh_(int absCh, bool isMain) {
  // 范围检查：绝对通道 9..(9+MAXIPCNUM-1)
  if (absCh < IPCCHANNELBEGIN || absCh >= IPCCHANNELBEGIN + MAXIPCNUM)
    return -1;

  // 已有就直接返回
  if (GetVDecChn(absCh) >= 0) return 0;

  // 若该通道当前无效/未配置，没必要绑定（可根据你系统改掉这句）
  if (!IsValid((uint8_t)absCh, isMain)) {
    MSLOG_WARN("EnsureVdecForAbsCh_: absCh=%d is not valid/online, skip bind", absCh);
    return -2;
  }

  // 先尝试绑定一次（**注意**：这个函数常常是异步完成）
  int32_t one = absCh;          // 一定要传“绝对通道号”
  if (BindVdec(1, &one) != 0) {
    MSLOG_ERROR("EnsureVdecForAbsCh_: BindVdec absCh=%d failed", absCh);
    return -3;
  }

  // 绑定后等待：1) 映射表出现；2) 解码器至少收到一帧（可选，但强烈建议）
  const int base = absCh - IPCCHANNELBEGIN;
  const int idx  = base;        // 你只用主码流；若以后要子码流这里改成 base+MAXIPCNUM
  const int WAIT_MS_MAP  = 1200; // 等映射插入
  const int WAIT_MS_FRAME= 800;  // 等第一帧
  long long deadline = local_get_curtime() + (long long)WAIT_MS_MAP * 1000;

  // 轮询映射（m_mapIpcVdec 可能在回调 / 初始化完成后才写入）
  while (local_get_curtime() < deadline) {
    if (GetVDecChn(absCh) >= 0) break;
    usleep(50 * 1000);
  }

  int vdec = GetVDecChn(absCh);
  if (vdec < 0) {
    MSLOG_ERROR("EnsureVdecForAbsCh_: after BindVdec, still no VDEC for absCh=%d", absCh);
    DumpVdecMap_();   // 打印当前表，看看有没有用错 key
    return -4;
  }

  // 可选：再等一帧，彻底避免 RGA 源 0Bytes
  (void)WaitVdecReadyIdx_(idx, WAIT_MS_FRAME);

  return 0;
}


// --- 等待内部 idx（主0..7，子8..15）收到新帧 ---
bool CIpCamManage::WaitVdecReadyIdx_(int idx, int wait_ms) {
  if (idx < 0 || idx >= MAXIPCNUM * 2) return false;
  long long first = m_updatetime[idx];
  long long deadline = local_get_curtime() + (long long)wait_ms * 1000;
  while (local_get_curtime() < deadline) {
    if (m_updatetime[idx] != first) return true;
    usleep(50 * 1000);
  }
  return false;
}

int CIpCamManage::SnapshotJpegMulti(const int *absChs, int n, bool /*isMain*/,
                                    const char *save_dir, int timeout_ms, int quality) {
  if (!absChs || n <= 0 || !save_dir || !save_dir[0]) {
    MSLOG_ERROR("SnapshotJpegMulti: invalid args");
    return -1;
  }

  auto clamp_q = [](int q){ return (q < 1) ? 1 : (q > 100 ? 100 : q); };
  const int qf = clamp_q(quality);

  int ok = 0;
  for (int i = 0; i < n; ++i) {
    const int absCh = absChs[i];
    if (absCh < IPCCHANNELBEGIN || absCh >= IPCCHANNELBEGIN + MAXIPCNUM) {
      MSLOG_WARN("SnapshotJpegMulti: drop invalid absCh=%d", absCh);
      continue;
    }

    // 1) 仅抓主码流，通道必须有效（在线）
    if (!IsValid((uint8_t)absCh, /*isMain=*/true)) {
      MSLOG_WARN("SnapshotJpegMulti: absCh=%d not valid/online, skip", absCh);
      continue;
    }

    // 2) 确保存在 VDEC：若没有，就只给这一通道临时 Bind 一次
    int vdecChn = GetVDecChn(absCh);
    if (vdecChn < 0) {
      int32_t one = absCh;
      if (BindVdec(1, &one) != 0) {
        MSLOG_ERROR("SnapshotJpegMulti: BindVdec fail absCh=%d", absCh);
        continue;
      }
      vdecChn = GetVDecChn(absCh);
      if (vdecChn < 0) {
        MSLOG_ERROR("SnapshotJpegMulti: still no VDEC after bind, absCh=%d", absCh);
        continue;
      }
    }

    // 3) 等主码流“真的出过帧”：双门闩 (GetVideoLost == false) 且 m_updatetime 变化
    const int idx = absCh - IPCCHANNELBEGIN; // 主码流索引 0..MAXIPCNUM-1
    long long first = m_updatetime[idx];
    bool ready = false;
    // 触发一次 IDR（如果你的回调里识别 m_u8NeedIFrame，则置位它）
    if (idx >= 0 && idx < MAXIPCNUM) m_u8NeedIFrame[idx] = 1;

    for (int t = 0; t < 20; ++t) { // 20*100ms ≈ 2s
      bool lost = GetVideoLost((uint8_t)absCh, /*isMain=*/true);
      if (!lost && m_updatetime[idx] != first) { ready = true; break; }
      usleep(100 * 1000);
    }
    if (!ready) {
      MSLOG_WARN("SnapshotJpegMulti: absCh=%d stream not ready (no new frame), skip", absCh);
      continue;
    }

    // 4) 查询分辨率
    uint32_t w = 0, h = 0;
    if (GetChnSize(absCh, /*isMain=*/true, &w, &h) < 0 || w == 0 || h == 0) {
      MSLOG_ERROR("SnapshotJpegMulti: GetChnSize fail absCh=%d", absCh);
      continue;
    }
    const uint32_t vw = (w + 15) & ~15u;
    const uint32_t vh = (h + 15) & ~15u;

    // 5) 封装一个“抓一帧”的 lambda（支持两种路径：直连 / 经 RGA）
    auto snap_once = [&](bool use_rga)->bool {
      VENC_CHN vencChn = -1;
      VENC_CHN_ATTR_S veAttr; memset(&veAttr, 0, sizeof(veAttr));
      veAttr.stVencAttr.enType       = RK_CODEC_TYPE_JPEG;
      veAttr.stVencAttr.imageType    = IMAGE_TYPE_NV12;
      veAttr.stVencAttr.u32PicWidth  = w;
      veAttr.stVencAttr.u32PicHeight = h;
      veAttr.stVencAttr.u32VirWidth  = vw;
      veAttr.stVencAttr.u32VirHeight = vh;

      for (int ch = VENC_MAX_CHN_NUM - 1; ch >= 0; --ch) {
        if (RK_MPI_VENC_CreateChn(ch, &veAttr) == 0) { vencChn = ch; break; }
        if (RK_MPI_VENC_CreateJpegLightChn(ch, &veAttr) == 0) { vencChn = ch; break; }
      }
      if (vencChn < 0) {
        MSLOG_ERROR("SnapshotJpegMulti: create VENC failed absCh=%d", absCh);
        return false;
      }

      VENC_JPEG_PARAM_S jpgParam; memset(&jpgParam, 0, sizeof(jpgParam));
      jpgParam.u32Qfactor = qf;
      RK_MPI_VENC_SetJpegParam(vencChn, &jpgParam);
      RK_MPI_SYS_SetMediaBufferDepth(RK_ID_VENC, vencChn, 2);
      RK_MPI_SYS_SetMediaBufferDepth(RK_ID_VDEC, vdecChn, 4);

      RGA_CHN rgaChn = -1;
      MPP_CHN_S src{}, mid{}, dst{};
      src.enModId = RK_ID_VDEC; src.s32ChnId = vdecChn;
      dst.enModId = RK_ID_VENC; dst.s32ChnId = vencChn;

      if (use_rga) {
        RGA_ATTR_S rgaAttr; memset(&rgaAttr, 0, sizeof(rgaAttr));
        rgaAttr.bEnBufPool    = RK_TRUE;
        rgaAttr.u16BufPoolCnt = 3;
        rgaAttr.u16Rotaion    = 0;
        rgaAttr.stImgIn.imgType       = IMAGE_TYPE_NV12;
        rgaAttr.stImgIn.u32Width      = w;
        rgaAttr.stImgIn.u32Height     = h;
        rgaAttr.stImgIn.u32HorStride  = vw;
        rgaAttr.stImgIn.u32VirStride  = vh;
        rgaAttr.stImgOut = rgaAttr.stImgIn;

        for (int ch = 0; ch < RGA_MAX_CHN_NUM; ++ch) {
          if (RK_MPI_RGA_CreateChn(ch, &rgaAttr) == 0) { rgaChn = ch; break; }
        }
        if (rgaChn < 0) {
          MSLOG_ERROR("SnapshotJpegMulti: create RGA failed absCh=%d", absCh);
          RK_MPI_VENC_DestroyChn(vencChn);
          return false;
        }
        mid.enModId = RK_ID_RGA; mid.s32ChnId = rgaChn;
        if (RK_MPI_SYS_Bind(&src, &mid) != 0) {
          MSLOG_ERROR("SnapshotJpegMulti: Bind VDEC(%d)->RGA(%d) failed", vdecChn, rgaChn);
          RK_MPI_RGA_DestroyChn(rgaChn);
          RK_MPI_VENC_DestroyChn(vencChn);
          return false;
        }
        if (RK_MPI_SYS_Bind(&mid, &dst) != 0) {
          MSLOG_ERROR("SnapshotJpegMulti: Bind RGA(%d)->VENC(%d) failed", rgaChn, vencChn);
          RK_MPI_SYS_UnBind(&src, &mid);
          RK_MPI_RGA_DestroyChn(rgaChn);
          RK_MPI_VENC_DestroyChn(vencChn);
          return false;
        }
      } else {
        // 直连：VDEC -> VENC
        if (RK_MPI_SYS_Bind(&src, &dst) != 0) {
          MSLOG_ERROR("SnapshotJpegMulti: Bind VDEC(%d)->VENC(%d) failed", vdecChn, vencChn);
          RK_MPI_VENC_DestroyChn(vencChn);
          return false;
        }
      }

      // 给链路更长的稳定时间（避免 0Bytes）
      usleep(use_rga ? 300 * 1000 : 200 * 1000);

      VENC_RECV_PIC_PARAM_S recvParam; recvParam.s32RecvPicNum = 1;
      RK_MPI_VENC_StartRecvFrame(vencChn, &recvParam);
      RK_MPI_SYS_StartGetMediaBuffer(RK_ID_VENC, vencChn);

      MEDIA_BUFFER out = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, vencChn, timeout_ms);
      if (!out) {
        MSLOG_ERROR("SnapshotJpegMulti: JPEG timeout absCh=%d after %d ms (use_rga=%d)",
                    absCh, timeout_ms, (int)use_rga);
        RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, vencChn);

        // 解绑/销毁
        if (use_rga) {
          RK_MPI_SYS_UnBind(&mid, &dst);
          RK_MPI_SYS_UnBind(&src, &mid);
          RK_MPI_RGA_DestroyChn(rgaChn);
        } else {
          RK_MPI_SYS_UnBind(&src, &dst);
        }
        RK_MPI_VENC_DestroyChn(vencChn);
        return false;
      }

      char jpg_path[256];
      std::snprintf(jpg_path, sizeof(jpg_path), "%s/ipc%d_main.jpg", save_dir, absCh);

      int fd = ::open(jpg_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
      if (fd < 0) {
        MSLOG_ERROR("SnapshotJpegMulti: open('%s') failed err=%d", jpg_path, errno);
        RK_MPI_MB_ReleaseBuffer(out);
        RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, vencChn);
        if (use_rga) {
          RK_MPI_SYS_UnBind(&mid, &dst);
          RK_MPI_SYS_UnBind(&src, &mid);
          RK_MPI_RGA_DestroyChn(rgaChn);
        } else {
          RK_MPI_SYS_UnBind(&src, &dst);
        }
        RK_MPI_VENC_DestroyChn(vencChn);
        return false;
      }

      void *ptr = RK_MPI_MB_GetPtr(out);
      size_t size = RK_MPI_MB_GetSize(out);
      ssize_t wr = ::write(fd, ptr, size);
      ::close(fd);

      RK_MPI_MB_ReleaseBuffer(out);
      RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, vencChn);

      if (use_rga) {
        RK_MPI_SYS_UnBind(&mid, &dst);
        RK_MPI_SYS_UnBind(&src, &mid);
        RK_MPI_RGA_DestroyChn(rgaChn);
      } else {
        RK_MPI_SYS_UnBind(&src, &dst);
      }
      RK_MPI_VENC_DestroyChn(vencChn);

      if (wr < 0 || (size_t)wr != size) {
        MSLOG_ERROR("SnapshotJpegMulti: write('%s') failed (%zd/%zu)", jpg_path, wr, size);
        return false;
      }

      MSLOG_INFO("SNAP OK: absCh=%d vdec=%d => %s (%ux%u q=%d)",
                 absCh, vdecChn, jpg_path, w, h, qf);
      return true;
    }; // snap_once

    // 6) 尝试两次：先直连，不行再走 RGA
    if (snap_once(/*use_rga=*/false) || snap_once(/*use_rga=*/true)) {
      ++ok;
    }
  }

  MSLOG_INFO("SnapshotJpegMulti done: total=%d, ok=%d", n, ok);
  return (ok > 0) ? 0 : -1;
}


void* CIpCamManage::IpcSnapThreadProc_(void *arg) {
  IpcSnapCtx *ctx = (IpcSnapCtx*)arg;
  CIpCamManage *self = ctx->self;

  const int absCh   = ctx->absCh;
  const int idx     = ctx->idx;
  const bool isMain = ctx->isMain;

  ctx->result = -1;

  int vdecChn = self->GetVDecChn(absCh);
  if (vdecChn < 0) {
    MSLOG_ERROR("IpcSnapThread: no VDEC for absCh=%d", absCh);
    return ctx;
  }

  uint32_t w=0,h=0;
  if (self->GetChnSize(absCh, isMain, &w, &h) < 0 || !w || !h) return ctx;
  const uint32_t vw = (w + 15) & ~15;
  const uint32_t vh = (h + 15) & ~15;

  // 等就绪，避免 0Bytes
  (void)self->WaitVdecReadyIdx_(idx, 1500);
  usleep(120 * 1000);

  // 创建一次性 VENC(JPEG)
  VENC_CHN_ATTR_S ve{}; ve.stVencAttr.enType=RK_CODEC_TYPE_JPEG; ve.stVencAttr.imageType=IMAGE_TYPE_NV12;
  ve.stVencAttr.u32PicWidth=w; ve.stVencAttr.u32PicHeight=h; ve.stVencAttr.u32VirWidth=vw; ve.stVencAttr.u32VirHeight=vh;
  VENC_CHN venc = -1;
  for (int ch = VENC_MAX_CHN_NUM - 1; ch >= 0; --ch) {
    if (RK_MPI_VENC_CreateChn(ch, &ve) == 0) { venc = ch; break; }
    if (RK_MPI_VENC_CreateJpegLightChn(ch, &ve) == 0) { venc = ch; break; }
  }
  if (venc < 0) return ctx;

  VENC_JPEG_PARAM_S jp{}; jp.u32Qfactor = std::max(1, std::min(100, ctx->quality));
  RK_MPI_VENC_SetJpegParam(venc, &jp);
  RK_MPI_SYS_SetMediaBufferDepth(RK_ID_VENC, venc, 2);
  RK_MPI_SYS_SetMediaBufferDepth(RK_ID_VDEC, vdecChn, 4);

  // RGA（必须开池，池深 >=3）
  RGA_ATTR_S ra{}; ra.bEnBufPool=RK_TRUE; ra.u16BufPoolCnt=3; ra.u16Rotaion=0;
  ra.stImgIn.imgType=IMAGE_TYPE_NV12;  ra.stImgIn.u32Width=w;  ra.stImgIn.u32Height=h;  ra.stImgIn.u32HorStride=vw;  ra.stImgIn.u32VirStride=vh;
  ra.stImgOut.imgType=IMAGE_TYPE_NV12; ra.stImgOut.u32Width=w; ra.stImgOut.u32Height=h; ra.stImgOut.u32HorStride=vw; ra.stImgOut.u32VirStride=vh;
  RGA_CHN rga=-1; for (int ch=0; ch < RGA_MAX_CHN_NUM; ++ch) if (RK_MPI_RGA_CreateChn(ch, &ra) == 0) { rga = ch; break; }
  if (rga < 0) { RK_MPI_VENC_DestroyChn(venc); return ctx; }

  // 绑定
  MPP_CHN_S src{RK_ID_VDEC, vdecChn}, mid{RK_ID_RGA, rga}, dst{RK_ID_VENC, venc};
  if (RK_MPI_SYS_Bind(&src, &mid) != 0) { RK_MPI_RGA_DestroyChn(rga); RK_MPI_VENC_DestroyChn(venc); return ctx; }
  if (RK_MPI_SYS_Bind(&mid, &dst) != 0) { RK_MPI_SYS_UnBind(&src,&mid); RK_MPI_RGA_DestroyChn(rga); RK_MPI_VENC_DestroyChn(venc); return ctx; }

  usleep(120 * 1000);

  VENC_RECV_PIC_PARAM_S rp{}; rp.s32RecvPicNum=1;
  RK_MPI_VENC_StartRecvFrame(venc, &rp);
  RK_MPI_SYS_StartGetMediaBuffer(RK_ID_VENC, venc);
  MEDIA_BUFFER out = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, venc, ctx->timeout_ms);
  if (!out) {
    RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, venc);
    RK_MPI_SYS_UnBind(&mid,&dst); RK_MPI_SYS_UnBind(&src,&mid);
    RK_MPI_RGA_DestroyChn(rga); RK_MPI_VENC_DestroyChn(venc);
    return ctx;
  }

  int fd = ::open(ctx->jpg_path, O_CREAT|O_WRONLY|O_TRUNC, 0666);
  if (fd >= 0) {
    void* p = RK_MPI_MB_GetPtr(out); size_t sz = RK_MPI_MB_GetSize(out);
    ssize_t wr = ::write(fd, p, sz); (void)wr; ::close(fd);
  }
  RK_MPI_MB_ReleaseBuffer(out);
  RK_MPI_SYS_StopGetMediaBuffer(RK_ID_VENC, venc);

  RK_MPI_SYS_UnBind(&mid,&dst); RK_MPI_SYS_UnBind(&src,&mid);
  RK_MPI_RGA_DestroyChn(rga); RK_MPI_VENC_DestroyChn(venc);

  ctx->result = 0;
  return ctx;
}


通过以下接口调用
int rk_get_incident_images(const IncidentImageRequest_t *request,
                           IncidentImageResponse_t *response) {
  auto *mgr = CIpCamManage::Instance();

int chs[] = {9,10,11,12,13,14,15,16};
mgr->SnapshotJpegMulti(chs, 8, /*isMain=*/true, "/tmp", 5000, 90);


  // 这里你可以把生成的 /tmp/ipcX_main.jpg 回填到 response
  return 0;
}实现多IPC通道同时抓拍
