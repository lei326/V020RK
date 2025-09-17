#ifndef __DOORDVR_IPCAMMANAGE_H__
#define __DOORDVR_IPCAMMANAGE_H__
#include "doordvr_typesdefine.h"
#include "singletondefine.h"
#include "doordvr_ipcam.h"
#include "rkmedia_api.h"
#include "msmutex.h"
#include "doordvr_config.h"
//#ifdef __cplusplus
//#if __cplusplus
//extern "C"
//{
//#endif
//#endif /* End of #ifdef __cplusplus */
typedef struct
{
	uint8_t chn;
	uint8_t stream;			//0---主码流    1---子码流 
	uint8_t encoder;		//0---h264  1---h265
}UPDATE_IPC_ENCODER_PARAM;

#define MAXIPCNUM				IPCAM_CHANNEL_NUM
//IPCAM参数通道号的起始值
#define IPCCHANNELBEGIN		9
//IPCAM厂家类型
#define	IPCAM_XINRUISHI		0
#define IPCAM_DEV_TYPE		IPCAM_XINRUISHI
//私有函数的通道号从0开始，外部函数的通道号从IPCCHANNELBEGIN开始
class CIpCamManage
{
	SINGLETON_DECLARE(CIpCamManage);
public:
int SnapshotJpeg(uint8_t chn, bool isMain,
                 const char* jpg_path, int timeout_ms, int quality);

int SnapshotJpegMulti(const int *absChs, int n, bool isMain,
                      const char *save_dir /*例如 "/tmp"*/,
                      int timeout_ms /*例如 5000*/,
                      int quality    /*1~100*/);
					  
	int InitManageIpCam();
	int StartManageIpCam();
	int StopManageIpCam();
	bool GetVideoLost(uint8_t chn, bool isMain);
	//SIZE_S GetPicSize(uint8_t chn);
	//设置音频回调函数,音频同时只有一路音频可以实时播放
	int SetIpCamAudioStreamCallback(uint8_t chn, EncoderDataCallback cbfunc, void *pUsrData);
	int ClearIpCamAudioStreamCallback();
	
	//获取通道分辨率
	int GetChnSize(uint8_t chn, bool isMain, uint32_t *width, uint32_t *height);
	//获取通道帧率
	int GetChnFrameRate(uint8_t chn, bool isMain, uint32_t *frameRate);
	//获取通道是否带音
	int GetChnAudioFlag(uint8_t chn, bool isMain, uint32_t *audioFlag);
	//获得编码格式 0---h264,1---h265
	int GetEncoderFormat(uint8_t chn, bool isMain);
	//IPCAM 通道chn是否有有效对象，1--有效，0--无效
	bool IsValid(uint8_t chn, bool isMain);
	//创建VDEC并和相应的IPC码流绑定
	int BindVdec(int32_t chn_num,    int32_t* chn);
	static int IpCamCallback(int chn, int stream_type, FRAME_INFO_T *frame_buf, void *pUsrData);
	//获得IPCAM通道chn所绑定的VDEC通道号
	int GetVDecChn(int chn);
	//设置通道参数,chn==IPCAM_CHANNEL_NUM 全部通道参数
	int SetChnConfig(IPCamEncoderConfig_t* config, uint32_t chn);
	//清除所有视频预览回调函数
	int ClearAllIpCamVideoStreamCallback();
protected:
	static void* IpCheckVideoThread(void *pUsrData);
    void 	IpCheckVideoProcess();
    static void* checkIPCNetworkEnvironment(void *pUsrData);
    void    checkIPCNetworkEnvironmentProcess();
    static int IpcEncoderParamCallback(int chn, IPCAMSTREAM stream, IPC_ENCODER_PARAM_T *stIpcParam, void *pUsrData);
private:
int  NormalizeAbsCh_(int ch) const;   // 将 0-based / 9-based 统一成 9-based
int EnsureVdecForAbsCh_(int absCh, bool isMain);

struct IpcSnapCtx {
  CIpCamManage *self;
  int   absCh;       // 外部通道：9~16
  int   idx;         // 内部索引：主 chn = absCh-IPCCHANNELBEGIN；子= chn+MAXIPCNUM
  bool  isMain;
  char  jpg_path[256];
  int   timeout_ms;
  int   quality;
  int   result;      // 0=成功；<0 失败
};





void DumpVdecMap_() const;  
	//设置视频预览回调函数
	int SetIpCamVideoStreamCallback(uint8_t chn, EncoderDataCallback cbfunc, void *pUsrData);
	int IpCamCb(int chn, int stream_type, FRAME_INFO_T *frame_buf);
	//修改通道参数配置
	int ChangeChnConfig(IPCamEncoderConfig_t* config, uint32_t chn);
	int CreateIpCamStream(uint32_t chn);
	int DestroyIpCamStream(uint32_t chn);
	int StartIpCamStream(uint32_t chn);
	int StartIpCamStream(CIpCamStream* pSession, uint32_t chn);
private:

  static void* IpcSnapThreadProc_(void *arg);
  bool WaitVdecReadyIdx_(int idx, int wait_ms);
	typedef std::map<unsigned short, CIpCamStream*> 	mapIpCamStream;
	typedef mapIpCamStream::iterator		mapIpCamIter;

	CMsMutex 	m_lockIpCamStreamQuery;
	mapIpCamStream	m_mapIpCamStreamQuery;			//用于存放IPCAM实例和对应的通道号 通道号从9开始

	IPCamEncoderConfig_t m_IpCamConfig[MAXIPCNUM];		//IPCAM参数配置

	typedef std::map<unsigned short, unsigned short> 	mapIpCamVDec;
	typedef mapIpCamVDec::iterator		mapIpCamVDecIter;
	mapIpCamVDec m_mapIpcVdec;			//MAP <IPCAM通道号 从9开始(主码流和子码流共用同一数字)，VDEC通道号>

  CMsThread m_CheckIPCNetworkEnvironmentThread; //检查IPC网络是否就绪，就绪后退出

	CMsThread m_CheckVideoStreamThread;		//检测视频流是否停止线程
	CMsMutex 	m_lockIpCamPreviewBuffer[MAXIPCNUM];	//实时预览
	MEDIA_BUFFER  m_PreviewBuffer[MAXIPCNUM];				//实时预览
	long long m_updatetime[MAXIPCNUM];							//每个解码通道的实时预览更新时间
	uint8_t m_u8NeedIFrame[MAXIPCNUM];													//重新绑定后第一帧位I帧
};
//#ifdef __cplusplus
//#if __cplusplus
//}
//#endif
//#endif /* End of #ifdef __cplusplus */

#endif
