#include "doordvr_export.h"
#include "tools.h"
#include "doordvr_ipcam.h"
#include "my_pub_common.h"
#include "PUB_common.h"
#include "FramePackage.h"
#include <string>
#include "rtmp_ms.h"   
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 500000

CIpCamStream::CIpCamStream(const string RtspUri, DVR_U8_T channel, IPCAMSTREAM stream, bool haveAudio, uint32_t width, uint32_t height, uint32_t frameRate, uint32_t encoderType)
{
	m_StreamType = stream;
	if(m_StreamType == IPCAM_SUB_STREAM)
		m_IsWithAudio = false;
	else
		m_IsWithAudio = haveAudio;
	//m_IsWithAudio = false;
	//m_uDevType = devtype;
	m_width = width;
	m_height = height;
	m_frameRate = frameRate;
	m_StreamStart = false;
	m_dwFrameNumber = 0;
	m_dwAudioFrameNumber = 0;
	m_videolost = true;
	m_channel= channel;
	if(RtspUri.empty()) return;
	m_RtspUri.assign(RtspUri);
	scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);
	iter = NULL;
	session = NULL;
	subsession = NULL;
	ourClient = NULL;
	ourAuthenticator = NULL;
	eventLoopWatchVariable = 0;
	m_fnVideoEncoderData = NULL;
	m_pVideoUserData = NULL;
	m_fnAudioEncoderData = NULL;
	m_pAudioUserData = NULL;
	m_rtspStatus = RTSP_DISCONNECT;
	m_VideoDataTime = 0;
	m_AudioDataTime = 0;
	m_uEncoderType = encoderType;
	MSLOG_DEBUG("CIpCamStream chn(%d,%d) Encoder:%d WithAudio%d,url:%s", channel, stream, m_uEncoderType, m_IsWithAudio, RtspUri.c_str());
}

CIpCamStream::~CIpCamStream()
{
	env->reclaim(); env = NULL;
  delete scheduler; scheduler = NULL;
}

/***********************************************************************************************************
**函数:
**输入参数:
**功能:设置IPCAM用户名和密码
**返回值:成功--0
***********************************************************************************************************/
int CIpCamStream::SetUserNamePwd(const string username, const string userpwd)
{
	//if(username.empty() && userpwd.empty()) return -1;

	m_UserName.assign(username);
	m_Password.assign(userpwd);
	return 0;
}

#if 0
/***********************************************************************************************************
**函数:
**输入参数:
**功能:设置IPCAM密码
**返回值:成功--0
***********************************************************************************************************/
int CIpCamStream::SetUserPwd(const string userpwd)
{
	if(userpwd.empty()) return -1;

	m_Password.assign(userpwd);
	return 0;
}
#endif

int CIpCamStream::IpCamStart()
{
	if(m_CheckThread.IsRunning() || m_ConnectThread.IsRunning())
	{
		return -1;
	}
	
	if(ourAuthenticator)
	{
		ourAuthenticator->setUsernameAndPassword(m_UserName.c_str(), m_Password.c_str());
	}
	else
	{
		ourAuthenticator = new Authenticator(m_UserName.c_str(), m_Password.c_str());
	}
/*	
	ourClient = ourRTSPClient::createNew(*env, m_RtspUri.c_str(), RTSP_CLIENT_VERBOSITY_LEVEL);
	if (ourClient == NULL) {
		MSLOG_ERROR("Failed to create a RTSP client for URL \"%s\":%s", m_RtspUri.c_str(), env->getResultMsg());
		return -1;
  }
	ourClient->miscPtr = this;
*/	
	if(StartRtsp() < 0)
	{
		return -1;
	}

	eventLoopWatchVariable = 0;
	m_ConnectThread.StartThread(IpCamConnectThread, this);
	m_CheckThread.StartThread(IpCamCheckThread, this);
	
	return 0;
}

bool CIpCamStream::IpCamIsStart()
{
	if(m_ConnectThread.IsRunning())
	{
		return true;
	}
	return false;
}

int CIpCamStream::SetIpCamVideoStreamCallback(EncoderDataCallback cbfunc, void *pUsrData)
{
	m_fnVideoEncoderData = cbfunc;
	m_pVideoUserData = pUsrData;
	return 0;
}

int CIpCamStream::ClearIpCamVideoStreamCallback()
{
	m_fnVideoEncoderData = NULL;
	m_pVideoUserData = NULL;
	return 0;
}

int CIpCamStream::SetIpCamAudioStreamCallback(EncoderDataCallback cbfunc, void *pUsrData)
{
	m_fnAudioEncoderData = cbfunc;
	m_pAudioUserData = pUsrData;
	return 0;
}

int CIpCamStream::ClearIpCamAudioStreamCallback()
{
	m_fnAudioEncoderData = NULL;
	m_pAudioUserData = NULL;
	return 0;
}

int CIpCamStream::SetIpCamEncoderParamCallback(EncoderParamCallback cbfunc, void *pUsrData)
{
	m_fnEncoderParamCb = cbfunc;
	m_pEncoderParamUserData = pUsrData;
	return 0;
}

int CIpCamStream::ClearIpCamEncoderParamCallback()
{
	m_fnEncoderParamCb = NULL;
	m_pEncoderParamUserData = NULL;
	return 0;
}

int CIpCamStream::IpCamStop()
{
	int ret = 0;
	if (!m_CheckThread.IsRunning() || !m_ConnectThread.IsRunning())
	{
		return -1;
	}

	m_CheckThread.StopThread();

	eventLoopWatchVariable = 1;
	nullTask = env->taskScheduler().scheduleDelayedTask(1000, (TaskFunc*)nullHandler, this);

	m_ConnectThread.StopThread();
	ret = StopRtsp();

	delete ourAuthenticator;
	return ret;
}

int CIpCamStream::StartRtsp()
{
	ourClient = ourRTSPClient::createNew(*env, m_RtspUri.c_str(), RTSP_CLIENT_VERBOSITY_LEVEL);
	if (ourClient == NULL) {
		MSLOG_ERROR("Failed to create a RTSP client for URL \"%s\":%s", m_RtspUri.c_str(), env->getResultMsg());
		return -1;
  }
	m_rtspStatus = RTSP_DESCRIBE;
	m_rtspStatusTime = GetTickCount();

	ourClient->miscPtr = this;
	ourClient->sendDescribeCommand(continueAfterDESCRIBE, ourAuthenticator); 
	return 0;
}

int CIpCamStream::StopRtsp()
{	
	shutdownStream(ourClient);
	ourClient = NULL;
	return 0;
}

void CIpCamStream::continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString)
{
	CIpCamStream* pThis = (CIpCamStream*)(((ourRTSPClient*)rtspClient)->miscPtr);
	pThis->ContinueAfterDESCRIBE(resultCode, resultString);
}

void CIpCamStream::ContinueAfterDESCRIBE(int resultCode, char* resultString)
{
  do {
    if (resultCode != 0) {
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
		MSLOG_INFO("CHN[%d,%d]: Got a SDP description:\n %s",m_channel, m_StreamType, sdpDescription);

    session = MediaSession::createNew(*env, sdpDescription);
    delete[] sdpDescription;
    if (session == NULL) {
			MSLOG_ERROR("CHN[%d,%d]: Failed to create a MediaSession: %s",m_channel, m_StreamType, env->getResultMsg());
      break;
    } else if (!session->hasSubsessions()) {
			MSLOG_ERROR("CHN[%d,%d]: This session has no subsessions",m_channel, m_StreamType);
      break;
    }

    iter = new MediaSubsessionIterator(*session);
    setupNextSubsession(ourClient);
    return;
  } while (0);

  shutdownStream(ourClient);
	ourClient = NULL;
}

void CIpCamStream::setupNextSubsession(RTSPClient* rtspClient) 
{
  subsession = iter->next();
  if (subsession != NULL) {
    if (!subsession->initiate()) {
			MSLOG_ERROR("CHN[%d,%d]: Failed to initiate \"%s/%s\": %s",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), env->getResultMsg());
      setupNextSubsession(rtspClient);
    } else {
			MSLOG_INFO("CHN[%d,%d]: Initiated \"%s/%s\" (",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName());
      if (subsession->rtcpIsMuxed()) {
				MSLOG_INFO("client port %d)", subsession->clientPortNum());
      } else {
      	MSLOG_INFO("client ports %d-%d)", subsession->clientPortNum(), subsession->clientPortNum()+1);
      }
			m_rtspStatus = RTSP_SETUP;
      m_rtspStatusTime = GetTickCount();

      rtspClient->sendSetupCommand(*subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP, False, ourAuthenticator);
    }
    return;
  }

	m_rtspStatus = RTSP_PLAY;
	m_rtspStatusTime = GetTickCount();	
  rtspClient->sendPlayCommand(*session, continueAfterPLAY, 0.0f, 0.0f, 1.0f, ourAuthenticator);
}

void CIpCamStream::continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString)
{
	CIpCamStream* pThis = (CIpCamStream*)(((ourRTSPClient*)rtspClient)->miscPtr);
	pThis->ContinueAfterSETUP(resultCode, resultString);
}

void CIpCamStream::ContinueAfterSETUP(int resultCode, char* resultString) 
{
  do {
    if (resultCode != 0) {
			printf("CHN[%d,%d]: Failed to set up \"%s/%s\": %s",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), resultString);
      break;
    }

    if (subsession->rtcpIsMuxed()) {
			MSLOG_INFO("CHN[%d,%d]: Set up \"%s/%s\" (client port %d)", 
				m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), subsession->clientPortNum());
    } else {		
			MSLOG_INFO("CHN[%d,%d]: Set up \"%s/%s\" (client ports %d-%d)", 
				m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), subsession->clientPortNum(), subsession->clientPortNum()+1);
    }

    if(strcmp(subsession->mediumName(), "audio") == 0)
    {
        // —— 把 SDP 的 PCMU/PCMA 映射到 RTMP 通道的 G711U/G711A（m_channel: 1-based -> RTMP: 0-based）
        const char* cname = subsession->codecName();
        int rtmp_ch = m_channel - 1;
        if (cname) {
#ifdef _WIN32
            auto ieq = [](const char* a, const char* b){ return _stricmp(a,b)==0; };
#else
            auto ieq = [](const char* a, const char* b){ return strcasecmp(a,b)==0; };
#endif
            if (ieq(cname, "PCMU") || ieq(cname, "G711U") || ieq(cname, "ULAW") || ieq(cname, "U-LAW")) {
                ms_rtmp_set_channel_audio_codec(rtmp_ch, "G711U");
                MSLOG_INFO("CHN[%d,%d]: audio codec mapped to G711U for RTMP ch=%d", m_channel, m_StreamType, rtmp_ch);
            } else if (ieq(cname, "PCMA") || ieq(cname, "G711A") || ieq(cname, "ALAW") || ieq(cname, "A-LAW")) {
                ms_rtmp_set_channel_audio_codec(rtmp_ch, "G711A");
                MSLOG_INFO("CHN[%d,%d]: audio codec mapped to G711A for RTMP ch=%d", m_channel, m_StreamType, rtmp_ch);
            } else {
                MSLOG_INFO("CHN[%d,%d]: audio codec \"%s\" not mapped (keep default)", m_channel, m_StreamType, cname ? cname : "-");
            }
        }
        subsession->sink = DummySink::createNew(*env, *subsession, ourClient->url(), audeoDataCallback, this);
    }
    else if(strcmp(subsession->mediumName(), "video") == 0)
    {
        if(strstr(subsession->codecName(), "265"))
        {
            if(m_uEncoderType != 1)
            {
                m_uEncoderType = 1;
                if(m_fnEncoderParamCb)
                {
                    IPC_ENCODER_PARAM_T param;
                    param.uEncoderType = m_uEncoderType;
                    m_fnEncoderParamCb(m_channel, m_StreamType, &param, m_pEncoderParamUserData);
                }
            }
        }
        else
        {
            if(m_uEncoderType != 0)
            {
                m_uEncoderType = 0;
                if(m_fnEncoderParamCb)
                {
                    IPC_ENCODER_PARAM_T param;
                    param.uEncoderType = m_uEncoderType;
                    m_fnEncoderParamCb(m_channel, m_StreamType, &param, m_pEncoderParamUserData);
                }
            }
        }
    	subsession->sink = DummySink::createNew(*env, *subsession, ourClient->url(), videoDataCallback, this);
    }

    if (subsession->sink == NULL) {		
			MSLOG_ERROR("CHN[%d,%d]: Failed to create sink for \"%s/%s\": %s",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), env->getResultMsg());
      break;
    }

		MSLOG_INFO("CHN[%d,%d]: Created sink for \"%s/%s\"",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName());
    subsession->miscPtr = ourClient;
    subsession->sink->startPlaying(*(subsession->readSource()),  subsessionAfterPlaying, subsession);

    if (subsession->rtcpInstance() != NULL) {
      subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, subsession);
    }
  } while (0);
  delete[] resultString;

  setupNextSubsession(ourClient);
}

void CIpCamStream::continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) 
{
	CIpCamStream* pThis = (CIpCamStream*)(((ourRTSPClient*)rtspClient)->miscPtr);
	pThis->ContinueAfterPLAY(resultCode, resultString);
}

void CIpCamStream::ContinueAfterPLAY(int resultCode, char* resultString) 
{
  Boolean success = False;

  do {
    if (resultCode != 0) {		
			printf("CHN[%d,%d]: Failed to start playing session: %s",m_channel, m_StreamType, resultString);
      break;
    }		
		MSLOG_INFO("CHN[%d,%d]: Started playing session...",m_channel, m_StreamType);
    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
    shutdownStream(ourClient);
		ourClient = NULL;
  }
}

void CIpCamStream::subsessionAfterPlaying(void* clientData) 
{
	MediaSubsession* subsession = (MediaSubsession*)clientData;
  CIpCamStream* pThis = (CIpCamStream*)(subsession->miscPtr);
	pThis->SubsessionAfterPlaying(subsession);
}

void CIpCamStream::SubsessionAfterPlaying(MediaSubsession* subsession)
{
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return;
  }

  shutdownStream(ourClient);
	ourClient = NULL;
}

void CIpCamStream::subsessionByeHandler(void* clientData)
{
	MediaSubsession* subsession = (MediaSubsession*)clientData;
  CIpCamStream* pThis = (CIpCamStream*)(subsession->miscPtr);
	pThis->subsessionByeHandler(subsession);
}

void CIpCamStream::SubsessionByeHandler(MediaSubsession* subsession) 
{
	MSLOG_INFO("CHN[%d,%d]: Received RTCP \"BYE\" on \"%s/%s\"", m_channel, m_StreamType, subsession->mediumName(), subsession->codecName());
  SubsessionAfterPlaying(subsession);
}

void CIpCamStream::shutdownStream(RTSPClient* rtspClient, int /*exitCode*/) {
	if(rtspClient == NULL) return;

  if (session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsession* sub_session;
		iter->reset();
    while ((sub_session = iter->next()) != NULL) {
      if (sub_session->sink != NULL) {
				Medium::close(sub_session->sink);
				sub_session->sink = NULL;

				if (sub_session->rtcpInstance() != NULL) {
				  sub_session->rtcpInstance()->setByeHandler(NULL, NULL);
				}

				someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
      rtspClient->sendTeardownCommand(*session, NULL);
    }

		delete iter;
		if (session != NULL) {
			env->taskScheduler().unscheduleDelayedTask(nullTask);
			Medium::close(session);
			session = NULL;
		}
  }
  Medium::close(rtspClient);

	m_videolost = true;
	m_StreamStart = false;
	m_rtspStatus = RTSP_ERROR;
	m_rtspStatusTime = GetTickCount();
}

void* CIpCamStream::IpCamConnectThread(void *pUsrData)
{
	CIpCamStream* pThis = (CIpCamStream*)pUsrData;
	pThis->IpCamConnectProcess();
	return 0;
}

void 	CIpCamStream::IpCamConnectProcess()
{
	MSLOG_DEBUG("start IpCamConnectProcess(%d,%d) thread running........thread pid = %lu",m_channel,m_StreamType,m_ConnectThread.GetThreadId());
	env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
	MSLOG_DEBUG("stop IpCamConnectProcess(%d,%d) thread running........thread pid = %u",m_channel,m_StreamType,(unsigned int)syscall(224));
}

void* CIpCamStream::IpCamCheckThread(void *pUsrData)
{
	CIpCamStream* pThis = (CIpCamStream*)pUsrData;
	pThis->IpCamCheckProcess();
	return 0;
}

void 	CIpCamStream::IpCamCheckProcess()
{
	MSLOG_DEBUG("start IpCamCheckProcess(%d,%d) thread running........thread pid = %lu",m_channel,m_StreamType,m_ConnectThread.GetThreadId());
	while(!m_CheckThread.GetExit())
	{
		int sta = 0;
		unsigned long long tick = GetTickCount();
		if(m_rtspStatus == RTSP_PLAYING)
		{
			if((tick > m_VideoDataTime) && (tick - m_VideoDataTime > 5000))
			{
				sta = 1;
				MSLOG_DEBUG("Ipcam video stream restart! %llu-%llu = %d", tick, m_VideoDataTime, tick - m_VideoDataTime);
			}
		}
		else if(m_rtspStatus == RTSP_ERROR)
		{		
			if((tick > m_rtspStatusTime) && (tick - m_rtspStatusTime > 4000))
			{
				sta = 1;
				MSLOG_DEBUG("Ipcam rtsp status restart!");
			}		
		}
		if(sta)
		{
			MSLOG_DEBUG("Ipcam %d restart!", m_channel);
			StartRtsp();
		}
		sleep(1);
	}
	
	MSLOG_DEBUG("stop IpCamCheckProcess(%d,%d) thread running........thread pid = %lu",m_channel,m_StreamType,m_ConnectThread.GetThreadId());
}

int CIpCamStream::GetPicSize(uint32_t* width, uint32_t* height)
{
	*width = m_width;
	*height = m_height;
	return 0;
}

int CIpCamStream::GetFrameRate(uint32_t* framerate)
{
	*framerate = m_frameRate;
	return 0;
}

int CIpCamStream::GetAudioFlag(uint32_t* audioflag)
{
	*audioflag  = m_IsWithAudio;
	return 0;
}

int CIpCamStream::videoDataCallback(uint8_t* buffer, uint32_t bufferSize, void *pUsrData)
{
	CIpCamStream* pThis = (CIpCamStream*)pUsrData;
	pThis->VideoDataCallback(buffer, bufferSize);
	return 0;
}

int CIpCamStream::VideoDataCallback(uint8_t* buffer, uint32_t bufferSize)
{
	FRAME_INFO_T frame_info={0};
	long long  current_time = local_get_curtime();
	
	m_VideoDataTime = GetTickCount();
	m_rtspStatus = RTSP_PLAYING;

	if(m_uEncoderType == 0)
	{
		if(buffer[4] == 0x67)
		{
			m_StreamStart = true;
			frame_info.keyFrame = 1;
		}
	}
	else
	{
		if(buffer[4] == 0x40 && buffer[5] == 0x01)
		{
			m_StreamStart = true;
			frame_info.keyFrame = 1;
		}
	}	
	
	if(m_StreamStart == true)
	{
		m_videolost = false;
		frame_info.channel = m_channel - 1;
		frame_info.frameType = frame_info.keyFrame ? 0 : 1;
    frame_info.width = m_width;
		frame_info.height = m_height;  
		frame_info.frame_id=m_dwFrameNumber++;
		frame_info.time=current_time;
		frame_info.relativeTime = current_time;
		frame_info.length=bufferSize;
		frame_info.pData = buffer;
		frame_info.encoder_format = m_uEncoderType;

		if(m_StreamType == IPCAM_MAIN_STREAM)
		{
			Hqt_Mpi_PutVideoFrame(0,  RECVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
			Hqt_Mpi_PutVideoFrame(m_channel - 1,  IPCAMNETMAINVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
			Hqt_Mpi_PutNetMainVideoFrame(m_channel - 1, NETMAINVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
			Hqt_Mpi_PutVideoFrame(0,  PRERECVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
		}
		else
		{
			Hqt_Mpi_PutVideoFrame(m_channel - 1,  IPCAMNETSUBVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
		}
		if(m_fnVideoEncoderData)
			m_fnVideoEncoderData(m_channel, frame_info.frameType, &frame_info, m_pVideoUserData);
	}

	return 0;
}

int CIpCamStream::audeoDataCallback(uint8_t* buffer, uint32_t bufferSize, void *pUsrData)
{
	CIpCamStream* pThis = (CIpCamStream*)pUsrData;
	pThis->AudeoDataCallback(buffer, bufferSize);
	return 0;
}

int CIpCamStream::AudeoDataCallback(uint8_t* buffer, uint32_t bufferSize)
{
	FRAME_INFO_T frame_info={0};
	long long  current_time = local_get_curtime();
	if(m_StreamType == IPCAM_MAIN_STREAM)
	{
		frame_info.channel = m_channel - 1;
		frame_info.frameType = 2;		// FRAME_TYPE_AUDIO
		frame_info.frame_id = m_dwAudioFrameNumber++;
		frame_info.time = current_time;
		frame_info.relativeTime = current_time;
		frame_info.length = bufferSize;
		frame_info.pData = buffer;

		// 录音/预录音（保持原行为）
		Hqt_Mpi_PutRecAudioFrame(m_channel - 1, &frame_info);
		Hqt_Mpi_PutPreRecAudioFrame(m_channel - 1, &frame_info);

		// 关键改动：除了投递到 IPCAMNETMAINVIDEO_STREAM_ID，再投递到 NETMAINVIDEO_STREAM_ID
		// 这样 RTMP 发送线程从 GetVideoFrame(channel, 1, ...) 就能取到音频帧 (frameType==2)
		Hqt_Mpi_PutVideoFrame(m_channel - 1,  IPCAMNETMAINVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
		Hqt_Mpi_PutNetMainVideoFrame(m_channel - 1, NETMAINVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);

		if(m_fnAudioEncoderData)
			m_fnAudioEncoderData(m_channel, frame_info.frameType, &frame_info, m_pAudioUserData);
	}

	return 0;
}

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
					int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
			     int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env,rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1) {
}

ourRTSPClient::~ourRTSPClient() {
}

DummySink* DummySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, SinkDataCallback cbfunc, void *pUsrData) {
  return new DummySink(env, subsession, streamId, cbfunc, pUsrData);
}

DummySink::DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, SinkDataCallback cbfunc, void *pUsrData)
  : MediaSink(env),
    fSubsession(subsession),m_fnSinkDataCallback(cbfunc), m_pSinkUserData(pUsrData) {
  fStreamId = strDup(streamId);
  fReceiveBuffer = new uint8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];
	bufferLen = 0;
	if(strcmp(fSubsession.mediumName(), "video") == 0)
	{
		fReceiveBuffer[bufferLen++] = 0x00;
		fReceiveBuffer[bufferLen++] = 0x00;
		fReceiveBuffer[bufferLen++] = 0x00;
		fReceiveBuffer[bufferLen++] = 0x01;	
	}
}

DummySink::~DummySink() {
  delete[] fReceiveBuffer;
  delete[] fStreamId;
}

void DummySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
  DummySink* sink = (DummySink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
	static unsigned s_log_cnt = 0;
  bool do_log = ((++s_log_cnt % 30) == 0);

  if (do_log) {
    if (strcmp(fSubsession.mediumName(), "video") == 0) {
      if (strcmp(fSubsession.codecName(), "H265") == 0) {
        int nal = (fReceiveBuffer[bufferLen] & 0x7E) >> 1;
        MSLOG_INFO("IPC[%s] VIDEO H265: size=%u, nal=%d, pts=%ld.%06ld",
                   fStreamId ? fStreamId : "-", frameSize, nal,
                   (long)presentationTime.tv_sec, (long)presentationTime.tv_usec);
      }
    }
  }

  if(m_fnSinkDataCallback)
  {
		if(strcmp(fSubsession.mediumName(), "audio") == 0)
		{
			m_fnSinkDataCallback(fReceiveBuffer, frameSize, m_pSinkUserData);
		}
		else if(strcmp(fSubsession.mediumName(), "video") == 0)
		{
			if(strcmp(fSubsession.codecName(), "H264") == 0)
			{
				int8_t ref = fReceiveBuffer[bufferLen] & 0x1f;
				switch(ref)
				{
					case 0x05:
						m_fnSinkDataCallback(fReceiveBuffer, frameSize + bufferLen, m_pSinkUserData);
						break;
					case 0x07:
						if(bufferLen == 4)
						{
							bufferLen += frameSize;
						}
						else
						{
							memcpy(fReceiveBuffer + 4, fReceiveBuffer + bufferLen, frameSize);
							bufferLen = 4 + frameSize;
						}
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x01;	
						break;
					case 0x08:
						bufferLen += frameSize;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x01;	
						break;
					default:
						m_fnSinkDataCallback(fReceiveBuffer + bufferLen - 4, frameSize + 4, m_pSinkUserData);
				}
			}
			else if(strcmp(fSubsession.codecName(), "H265") == 0)
			{
				int8_t ref = (fReceiveBuffer[bufferLen] & 0x7E) >> 1;
				switch(ref)
				{
					case 32:
						if(bufferLen == 4)
						{
							bufferLen += frameSize;
						}
						else
						{
							memcpy(fReceiveBuffer + 4, fReceiveBuffer + bufferLen, frameSize);
							bufferLen = 4 + frameSize;
						}
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x01;	
						break;
					case 33:
					case 34:
						bufferLen += frameSize;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x01;	
						break;
					case 19:
					case 20:
						m_fnSinkDataCallback(fReceiveBuffer, frameSize + bufferLen, m_pSinkUserData);
						break;
					default:
						m_fnSinkDataCallback(fReceiveBuffer + bufferLen - 4, frameSize + 4, m_pSinkUserData);
				}
			}	
		}
  }

	if (numTruncatedBytes > 0)
	{
		MSLOG_ERROR("%s/%s: %d bytes truncated", fSubsession.mediumName(), fSubsession.codecName(), numTruncatedBytes);
	}

  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False;

  fSource->getNextFrame(fReceiveBuffer + bufferLen, DUMMY_SINK_RECEIVE_BUFFER_SIZE - bufferLen,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}
