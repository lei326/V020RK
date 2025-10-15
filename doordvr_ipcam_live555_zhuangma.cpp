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
	//MSLOG_DEBUG("CHN%d, SetIpCamVideoStreamCallback", m_channel);
	m_fnVideoEncoderData = cbfunc;
	m_pVideoUserData = pUsrData;
	return 0;
}

int CIpCamStream::ClearIpCamVideoStreamCallback()
{
	//MSLOG_DEBUG("CHN%d, ClearIpCamVideoStreamCallback", m_channel);
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

/*	
	if(!m_UserName.empty() || !m_Password.empty())
	{
		delete ourAuthenticator;
		ourAuthenticator = new Authenticator(m_UserName.c_str(), m_Password.c_str());
	}
*/	
	ourClient = ourRTSPClient::createNew(*env, m_RtspUri.c_str(), RTSP_CLIENT_VERBOSITY_LEVEL);
	if (ourClient == NULL) {
		MSLOG_ERROR("Failed to create a RTSP client for URL \"%s\":%s", m_RtspUri.c_str(), env->getResultMsg());
		return -1;
  }
	m_rtspStatus = RTSP_DESCRIBE;
	m_rtspStatusTime = GetTickCount();
	// Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
	ourClient->miscPtr = this;
	ourClient->sendDescribeCommand(continueAfterDESCRIBE, ourAuthenticator); 

/*	
	if(!m_ConnectThread.IsRunning())
	{
		eventLoopWatchVariable = 0;
		m_ConnectThread.StartThread(IpCamConnectThread, this);
	}
*/	
	return 0;
}

int CIpCamStream::StopRtsp()
{	
/*	
	eventLoopWatchVariable = 1;
	nullTask = env->taskScheduler().scheduleDelayedTask(1000, (TaskFunc*)nullHandler, this);
	
	if (!m_ConnectThread.IsRunning())
	{
		return -1;
	}

	m_ConnectThread.StopThread();
*/	
	shutdownStream(ourClient);
	ourClient = NULL;

	return 0;
}

// Implementation of the RTSP 'response handlers':
void CIpCamStream::continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString)
{
	CIpCamStream* pThis = (CIpCamStream*)(((ourRTSPClient*)rtspClient)->miscPtr);
	pThis->ContinueAfterDESCRIBE(resultCode, resultString);
}

void CIpCamStream::ContinueAfterDESCRIBE(int resultCode, char* resultString)
{
  do {
    if (resultCode != 0) {
			//printf("CHN[%d,%d]: Failed to get a SDP description: %s\n",m_channel, m_StreamType, resultString);
      //env << *ourClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
		MSLOG_INFO("CHN[%d,%d]: Got a SDP description:\n %s",m_channel, m_StreamType, sdpDescription);
	//env << *ourClient << "Got a SDP description:\n" << sdpDescription << "\n";

	// Create a media session object from this SDP description:
    session = MediaSession::createNew(*env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (session == NULL) {
			MSLOG_ERROR("CHN[%d,%d]: Failed to create a MediaSession object from the SDP description:  %s",m_channel, m_StreamType, env->getResultMsg());
		//env << *ourClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    } else if (!session->hasSubsessions()) {
			MSLOG_ERROR("CHN[%d,%d]: This session has no subsessions",m_channel, m_StreamType);
		//env << *ourClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }
		//MSLOG_INFO("");
    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    iter = new MediaSubsessionIterator(*session);
		//MSLOG_INFO("");
    setupNextSubsession(ourClient);
    return;
  } while (0);

// An unrecoverable error occurred with this stream.
  shutdownStream(ourClient);
	ourClient = NULL;
}

void CIpCamStream::setupNextSubsession(RTSPClient* rtspClient) 
{
  subsession = iter->next();
  if (subsession != NULL) {
    if (!subsession->initiate()) {
			MSLOG_ERROR("CHN[%d,%d]: Failed to initiate \"%s/%s\": %s",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), env->getResultMsg());
		//env << *rtspClient << "Failed to initiate the \"" << *subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient);
    } else {
			MSLOG_INFO("CHN[%d,%d]: Initiated \"%s/%s\" (",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName());
		//env << *rtspClient << "Initiated the \"" << *subsession << "\" subsession (";
      if (subsession->rtcpIsMuxed()) {
				MSLOG_INFO("client port %d)", subsession->clientPortNum());
				//env << "client port " << subsession->clientPortNum();
      } else {
      	MSLOG_INFO("client ports %d-%d)", subsession->clientPortNum(), subsession->clientPortNum()+1);
				//env << "client ports " << subsession->clientPortNum() << "-" << subsession->clientPortNum()+1;
      }
	  		//MSLOG_INFO("");
			m_rtspStatus = RTSP_SETUP;
      m_rtspStatusTime = GetTickCount();
	  // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP, False, ourAuthenticator);

    }
    return;
  }

	m_rtspStatus = RTSP_PLAY;
	m_rtspStatusTime = GetTickCount();	
	// We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
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
			//env << *ourClient << "Failed to set up the \"" << *subsession << "\" subsession: " << resultString << "\n";
      break;
    }
	
	//env << *ourClient << "Set up the \"" << *subsession << "\" subsession (";
    if (subsession->rtcpIsMuxed()) {
			MSLOG_INFO("CHN[%d,%d]: Set up \"%s/%s\" (client port %d)", 
				m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), subsession->clientPortNum());
		//env << "client port " << subsession->clientPortNum();
    } else {		
			MSLOG_INFO("CHN[%d,%d]: Set up \"%s/%s\" (client ports %d-%d)", 
				m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), subsession->clientPortNum(), subsession->clientPortNum()+1);
		//env << "client ports " << subsession->clientPortNum() << "-" << subsession->clientPortNum()+1;
    }

	// Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)
    if(strcmp(subsession->mediumName(), "audio") == 0)
    {
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
                MSLOG_INFO("CHN[%d,%d]: audio codec not mapped", m_channel, m_StreamType, cname ? cname : "-");
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
	// perhaps use your own custom "MediaSink" subclass instead
    if (subsession->sink == NULL) {		
			MSLOG_ERROR("CHN[%d,%d]: Failed to create sink for \"%s/%s\": %s",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName(), env->getResultMsg());
			/*
      env << *ourClient << "Failed to create a data sink for the \"" << *subsession
	  << "\" subsession: " << env.getResultMsg() << "\n";
	  */
		break;
    }

		MSLOG_INFO("CHN[%d,%d]: Created sink for \"%s/%s\"",m_channel, m_StreamType, subsession->mediumName(), subsession->codecName());
		//env << *ourClient << "Created a data sink for the \"" << *subsession << "\" subsession\n";
    subsession->miscPtr = ourClient;// a hack to let subsession handler functions get the "RTSPClient" from the subsession 
    subsession->sink->startPlaying(*(subsession->readSource()),  subsessionAfterPlaying, subsession);
	// Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (subsession->rtcpInstance() != NULL) {
      subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, subsession);
    }
  } while (0);
  delete[] resultString;

  // Set up the next subsession, if any:
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
		//env << *ourClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }		
		MSLOG_INFO("CHN[%d,%d]: Started playing session...",m_channel, m_StreamType);
	//env << *ourClient << "Started playing session...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
	 // An unrecoverable error occurred with this stream.
    shutdownStream(ourClient);
		ourClient = NULL;
  }
}

// Implementation of the other event handlers:

void CIpCamStream::subsessionAfterPlaying(void* clientData) 
{
	MediaSubsession* subsession = (MediaSubsession*)clientData;
  CIpCamStream* pThis = (CIpCamStream*)(subsession->miscPtr);
	pThis->SubsessionAfterPlaying(subsession);
}

void CIpCamStream::SubsessionAfterPlaying(MediaSubsession* subsession)
{
  	//MediaSubsession* subsession = (MediaSubsession*)clientData;
  //RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return;// this subsession is still active
  }
  // All subsessions' streams have now been closed, so shutdown the client:
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
	  //MediaSubsession* subsession = (MediaSubsession*)clientData;
  //RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
  //UsageEnvironment& env = rtspClient->envir(); // alias
	MSLOG_INFO("CHN[%d,%d]: Received RTCP \"BYE\" on \"%s/%s\" subsession", m_channel, m_StreamType, subsession->mediumName(), subsession->codecName());
  
	  // Now act as if the subsession had closed:
	SubsessionAfterPlaying(subsession);
}

void CIpCamStream::shutdownStream(RTSPClient* rtspClient, int exitCode) {
	  //UsageEnvironment& env = rtspClient->envir(); // alias
  //StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias
	if(rtspClient == NULL) return;
  // First, check whether any subsessions have still to be closed:
  if (session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsession* sub_session;
		iter->reset();
    while ((sub_session = iter->next()) != NULL) {
      if (sub_session->sink != NULL) {
				Medium::close(sub_session->sink);
				sub_session->sink = NULL;

				if (sub_session->rtcpInstance() != NULL) {
				  sub_session->rtcpInstance()->setByeHandler(NULL, NULL);// in case the server sends a RTCP "BYE" while handling "TEARDOWN"
				}

				someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
	  // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the "TEARDOWN".
      rtspClient->sendTeardownCommand(*session, NULL);
    }

		delete iter;
		if (session != NULL) {
			// We also need to delete "session", and unschedule "streamTimerTask" (if set)
			env->taskScheduler().unscheduleDelayedTask(nullTask);
			Medium::close(session);
			session = NULL;
		}
  }
#if 0	
	if(rtspClient)
	{
		MSLOG_INFO("CHN[%d,%d]: Closing the stream.", m_channel, m_StreamType);
	}
#endif	
  //env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
	// Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.
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
	// All subsequent activity takes place within the event loop:
	//eventLoopWatchVariable = 1;
	//while(!m_ConnectThread.GetExit())
	{
	env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
	//MSLOG_DEBUG("");
	}
	//env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
 	// This function call does not return, unless, at some point in time, "eventLoopWatchVariable" gets set to something non-zero.
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
			//MSLOG_DEBUG("----%llu,%llu----", tick, m_VideoDataTime);
			if((tick > m_VideoDataTime) && (tick - m_VideoDataTime > 5000))
			{
				sta = 1;
				MSLOG_DEBUG("Ipcam video stream restart! %llu-%llu = %d", tick, m_VideoDataTime, tick - m_VideoDataTime);
			}
		}
		else if(m_rtspStatus == RTSP_ERROR)
		{		
			if((tick > m_rtspStatusTime) && (tick - m_rtspStatusTime > 4000))		//大于2s
			{
				sta = 1;
				MSLOG_DEBUG("Ipcam rtsp status restart!");
			}		
		}
		if(sta)
		{
			//StopRtsp();
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
	
	//修改视频数据获得时间
	m_VideoDataTime = GetTickCount();
	m_rtspStatus = RTSP_PLAYING;

#if 0	
	MSLOG_DEBUG("Video Chn%d:0x%x 0x%x 0x%x 0x%x 0x%x 0x%x", m_channel, 
		buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5]);
#endif
	//h264
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
		if(frame_info.keyFrame == 1)
      frame_info.frameType = 0;		//FRAME_TYPE_H264I;
    else
      frame_info.frameType = 1;		//FRAME_TYPE_H264P;	
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
			//MSLOG_DEBUG("CHN:%d,streamtype:%d,frameType:%d",m_channel, m_StreamType, frame_info.frameType);
		}
		else
		{
			Hqt_Mpi_PutVideoFrame(m_channel - 1,  IPCAMNETSUBVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
			//MSLOG_DEBUG("CHN%d,streamtype%d,m_fnVideoEncoderData%p",m_channel, m_StreamType, m_fnVideoEncoderData);
		}
		if(m_fnVideoEncoderData)
			m_fnVideoEncoderData(m_channel, frame_info.frameType, &frame_info, m_pVideoUserData);
		//m_IpCamDecoder->IpCamVdecSendStream(buf, size, false);
		//MSLOG_DEBUG("PutVideo%d (%d)", m_channel, frame_info.length);
		//MSLOG_DEBUG("CHN%d,streamtype%d,m_fnVideoEncoderData%p",m_channel, m_StreamType, m_fnVideoEncoderData);
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
		Hqt_Mpi_PutRecAudioFrame(m_channel - 1, &frame_info);
		Hqt_Mpi_PutPreRecAudioFrame(m_channel - 1, &frame_info);
		// Hqt_Mpi_PutTalkSendFrame(m_channel, &frame_info);
		Hqt_Mpi_PutVideoFrame(m_channel - 1,  IPCAMNETMAINVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);
		Hqt_Mpi_PutNetMainVideoFrame(m_channel - 1, NETMAINVIDEO_STREAM_ID, &frame_info, VIDEO_IPCAM);

		if(m_fnAudioEncoderData)
			m_fnAudioEncoderData(m_channel, frame_info.frameType, &frame_info, m_pAudioUserData);
			//MSLOG_DEBUG("------------------!!-------------!!!!!!!!!!!");
	}

	return 0;
}

// Implementation of "ourRTSPClient":

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

// Implementation of "DummySink":

// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:


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
#if 0	
		if(strcmp(fSubsession.codecName(), "H264") == 0)
		{
			sPropRecords = parseSPropParameterSets(fSubsession.fmtp_spropparametersets(), numSPropRecords);
		  for (unsigned int i = 0; i < numSPropRecords; ++i) 
			{
				if (sPropRecords[i].sPropLength > 0)
				{
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x01;
				}
				for(uint32_t j = 0; j < sPropRecords[i].sPropLength; j++)
				{
					fReceiveBuffer[bufferLen++] = sPropRecords[i].sPropBytes[j];
				}
		  }
			delete[] sPropRecords;
			fReceiveBuffer[bufferLen++] = 0x00;
			fReceiveBuffer[bufferLen++] = 0x00;
			fReceiveBuffer[bufferLen++] = 0x00;
			fReceiveBuffer[bufferLen++] = 0x01;
		}
		else if(strcmp(fSubsession.codecName(), "H265") == 0)
		{
			sPropRecords = parseSPropParameterSets(fSubsession.fmtp_spropvps(), numSPropRecords);
		  for (unsigned int i = 0; i < numSPropRecords; ++i) 
			{
				if (sPropRecords[i].sPropLength > 0)
				{
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x01;
				}
				for(uint32_t j = 0; j < sPropRecords[i].sPropLength; j++)
				{
					fReceiveBuffer[bufferLen++] = sPropRecords[i].sPropBytes[j];
				}
		  }
			delete[] sPropRecords;
			sPropRecords = parseSPropParameterSets(fSubsession.fmtp_spropsps(), numSPropRecords);
		  for (unsigned int i = 0; i < numSPropRecords; ++i) 
			{
				if (sPropRecords[i].sPropLength > 0)
				{
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x01;
				}
				for(uint32_t j = 0; j < sPropRecords[i].sPropLength; j++)
				{
					fReceiveBuffer[bufferLen++] = sPropRecords[i].sPropBytes[j];
				}
		  }
			delete[] sPropRecords;
			sPropRecords = parseSPropParameterSets(fSubsession.fmtp_sproppps(), numSPropRecords);
		  for (unsigned int i = 0; i < numSPropRecords; ++i) 
			{
				if (sPropRecords[i].sPropLength > 0)
				{
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x00;
					fReceiveBuffer[bufferLen++] = 0x01;
				}
				for(uint32_t j = 0; j < sPropRecords[i].sPropLength; j++)
				{
					fReceiveBuffer[bufferLen++] = sPropRecords[i].sPropBytes[j];
				}
		  }
			delete[] sPropRecords;
			fReceiveBuffer[bufferLen++] = 0x00;
			fReceiveBuffer[bufferLen++] = 0x00;
			fReceiveBuffer[bufferLen++] = 0x00;
			fReceiveBuffer[bufferLen++] = 0x01;
		}
#endif		
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

// If you don't want to see debugging output for each received frame, then comment out the following line:
//#define DEBUG_PRINT_EACH_RECEIVED_FRAME 0

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
	static unsigned s_log_cnt = 0;
  bool do_log = ((++s_log_cnt % 30) == 0);

  if (do_log) {
    if (strcmp(fSubsession.mediumName(), "video") == 0) {
      if (strcmp(fSubsession.codecName(), "H264") == 0) {
        int nal = fReceiveBuffer[bufferLen] & 0x1F;  // 取 H.264 NAL type
        // MSLOG_INFO("IPC[%s] VIDEO H264: size=%u, nal=%d, pts=%ld.%06ld",
        //            fStreamId ? fStreamId : "-", frameSize, nal,
        //            (long)presentationTime.tv_sec, (long)presentationTime.tv_usec);
      } else if (strcmp(fSubsession.codecName(), "H265") == 0) {
        int nal = (fReceiveBuffer[bufferLen] & 0x7E) >> 1; // 取 H.265 NAL type
        MSLOG_INFO("IPC[%s] VIDEO H265: size=%u, nal=%d, pts=%ld.%06ld",
                   fStreamId ? fStreamId : "-", frameSize, nal,
                   (long)presentationTime.tv_sec, (long)presentationTime.tv_usec);
      } else {
        MSLOG_INFO("IPC[%s] VIDEO %s: size=%u, pts=%ld.%06ld",
                   fStreamId ? fStreamId : "-", fSubsession.codecName(),
                   frameSize, (long)presentationTime.tv_sec, (long)presentationTime.tv_usec);
      }
    } else if (strcmp(fSubsession.mediumName(), "audio") == 0) {
    //   MSLOG_INFO("IPC[%s] AUDIO %s: size=%u, pts=%ld.%06ld",
    //              fStreamId ? fStreamId : "-", fSubsession.codecName(),
    //              frameSize, (long)presentationTime.tv_sec, (long)presentationTime.tv_usec);
    }
  }
	// We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
#endif
  //MSLOG_DEBUG("%s/%s:0x%x 0x%x 0x%x 0x%x 0x%x 0x%x", fSubsession.mediumName(), fSubsession.codecName(),
  //	fReceiveBuffer[0], fReceiveBuffer[1],fReceiveBuffer[2],fReceiveBuffer[3],fReceiveBuffer[4],fReceiveBuffer[5]);
  if(m_fnSinkDataCallback)
  {
		if(strcmp(fSubsession.mediumName(), "audio") == 0)
		{
			m_fnSinkDataCallback(fReceiveBuffer, frameSize, m_pSinkUserData);
		}
		else if(strcmp(fSubsession.mediumName(), "video") == 0)
		{
			//add 00 00 00 01
			if(strcmp(fSubsession.codecName(), "H264") == 0)
			{
				int8_t ref = fReceiveBuffer[bufferLen] & 0x1f;
				switch(ref)
				{
					case 0x05:
					//I帧
						m_fnSinkDataCallback(fReceiveBuffer, frameSize + bufferLen, m_pSinkUserData);
						break;
					case 0x07:		//sps
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
					case 0x08:		//pps
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
					case 32:		//vps
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
					case 33:		//sps
					case 34:		//pps
						bufferLen += frameSize;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x00;
						fReceiveBuffer[bufferLen++] = 0x01;	
						break;
					case 19:		//IDR
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
	// Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer + bufferLen, DUMMY_SINK_RECEIVE_BUFFER_SIZE - bufferLen,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}
