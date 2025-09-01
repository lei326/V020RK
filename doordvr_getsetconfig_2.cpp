#include <stddef.h>
#include "doordvr_export.h"
#include "testmain.h"
#include "intelligentdriverassistant.h"
#include "AdasDetect.h"
#include "IDAFrameDraw.h"
#include "rtmp_playback.h"
#include "server.h"
#include "bubiao1078apidemo.h"
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include "pcmPlayer.h"
#include "ms_netdvr_4gapi.h"
#include <inttypes.h>
// #ifdef __cplusplus
// #if __cplusplus
// extern "C" {
// #endif /* __cplusplus */
// #endif  /* __cplusplus */

/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
int g_cfg_dirty = 0;

#ifndef CFG_LOG_BUFSZ
#define CFG_LOG_BUFSZ 256
#endif

#define PERSIST_I64(dst, src)                                                      \
    do {                                                                           \
        int64_t _old__ = (int64_t)(dst);                                           \
        int64_t _new__ = (int64_t)(src);                                           \
        if (_old__ != _new__) {                                                    \
            (dst) = _new__;                                                        \
            g_cfg_dirty = 1;                                                       \
            char _logbuf[CFG_LOG_BUFSZ];                                           \
            snprintf(_logbuf, sizeof(_logbuf), "%s changed: %" PRId64 " -> %" PRId64, \
                     #dst, _old__, _new__);                                        \
            DVR_DEBUG(_logbuf);                                                    \
        }                                                                          \
    } while (0)

#define PERSIST_STR(dst, src)                                                      \
    do {                                                                           \
        size_t _n = sizeof(dst);                                                   \
        if (strncmp((dst), (src), _n) != 0) {                                      \
            strncpy((dst), (src), _n - 1);                                         \
            (dst)[_n - 1] = '\0';                                                  \
            g_cfg_dirty = 1;                                                       \
            char _logbuf[CFG_LOG_BUFSZ];                                           \
            snprintf(_logbuf, sizeof(_logbuf), "%s changed -> '%s'", #dst, (dst));\
            DVR_DEBUG(_logbuf);                                                    \
        }                                                                          \
    } while (0)

#define PERSIST_VAL(dst, src)                                                      \
    do {                                                                           \
        if ((dst) != (src)) {                                                      \
            (dst) = (src);                                                         \
            g_cfg_dirty = 1;                                                       \
            char _logbuf[CFG_LOG_BUFSZ];                                           \
            snprintf(_logbuf, sizeof(_logbuf), "%s changed", #dst);                \
            DVR_DEBUG(_logbuf);                                                    \
        }                                                                          \
    } while (0)

#define PERSIST_INT(dst, src)                                                         \
    do {                                                                              \
        int _old__ = (dst);                                                           \
        int _new__ = (src);                                                           \
        if (_old__ != _new__) {                                                       \
            (dst) = _new__;                                                           \
            g_cfg_dirty = 1;                                                          \
            char _buf__[256];                                                         \
            snprintf(_buf__, sizeof(_buf__), "%s changed: %d -> %d",                  \
                     #dst, _old__, _new__);                                           \
            DVR_DEBUG(_buf__);                                                        \
        }                                                                             \
    } while (0)

#define PERSIST_BOOL(dst, src)                                                        \
    do {                                                                              \
        int _old__ = !!(dst);                                                         \
        int _new__ = !!(src);                                                         \
        if (_old__ != _new__) {                                                       \
            (dst) = (bool)_new__;                                                     \
            g_cfg_dirty = 1;                                                          \
            char _buf__[256];                                                         \
            snprintf(_buf__, sizeof(_buf__), "%s changed: %s -> %s",                  \
                     #dst, _old__ ? "true" : "false", _new__ ? "true" : "false");     \
            DVR_DEBUG(_buf__);                                                        \
        }                                                                             \
    } while (0)

    #define SAFE_CPYI64(dst, src)      \
    do {                            \
        (dst) = (int64_t)(src);     \
    } while (0)


ABDParam_t g_abdParam;
extern DISK_RUN_STATE_E disk_run_state;
static DISK_RUN_STATE_E last_voice_state = DISK_STATE_BUTT;
DISK_FORMATING_STATE_E disk_format_state = DISK_FORMAT_STATE_BUTT;
DeviceConfig_t g_deviceconfig = {0};
pthread_mutex_t g_deviceconfig_mutex = PTHREAD_MUTEX_INITIALIZER;

static void DiskFormatStateCb(DISK_FORMATING_STATE_E state)
{
    printf("DiskFormatStateCb %d\n", state);
    disk_format_state = state;
}

typedef struct _ALLCONFIG_T
{
    AllConfigDef_t AllConfigDef; // 系统全局配置
    pthread_mutex_t config_lock;
    bool init_mutexLock;
} ALLCONFIG_T;
static ALLCONFIG_T allConfig = {.init_mutexLock = 0};


int SaveParam(void)
{
    const char *path = "/mnt/img/.hqtds";

    if (!g_cfg_dirty) {
        DVR_DEBUG("配置未改变，跳过写入");
        return 1; 
    }

    FILE *fp = fopen(path, "rb+");
    if (!fp) fp = fopen(path, "wb");
    if (!fp) {
        char buf[CFG_LOG_BUFSZ];
        snprintf(buf, sizeof(buf), "打开文件失败: %s", strerror(errno));
        DVR_DEBUG(buf);
        return -1;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) { DVR_DEBUG("fseek 失败"); fclose(fp); return -2; }
    if (fwrite(&allConfig.AllConfigDef, sizeof(AllConfigDef_t), 1, fp) != 1) {
        DVR_DEBUG("fwrite 失败"); fclose(fp); return -3;
    }
    if (fflush(fp) != 0) { DVR_DEBUG("fflush 失败"); fclose(fp); return -4; }
    int fd = fileno(fp);
    if (fd == -1 || fsync(fd) != 0) { DVR_DEBUG("fsync 失败"); fclose(fp); return -5; }
    if (fclose(fp) != 0) { DVR_DEBUG("fclose 失败"); return -6; }
    g_cfg_dirty = 0; 
    DVR_DEBUG("配置写入完成");
    return 0; 
}

void ensure_timezone_anchor_file()
{
    char last_tz[32] = {0};
    int ret = load_last_timezone_code(last_tz, sizeof(last_tz));
    int valid = 1;

    if (ret != 0)
    {
        valid = 0;
    }
    else
    {
        // 检查内容是否合理
        if (strlen(last_tz) < 6 || parse_timezone_offset(last_tz) == 0)
        {
            valid = 0;
        }
    }

    if (!valid)
    {
        const char *gmt_from_config = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneCode;
        save_last_timezone_code(gmt_from_config);
        printf("[TIMEZONE] 锚点文件丢失或无效，已用配置的时区 %s 创建/修复锚点。\n", gmt_from_config);
    }
}
void CleanUpLocalParam(void)
{
    ABDParam_t *abdparam = &allConfig.AllConfigDef.abdparam;
    if (abdparam->IdaCfgSet.drivlist.drivers != NULL)
    {
        free(abdparam->IdaCfgSet.drivlist.drivers);
        abdparam->IdaCfgSet.drivlist.drivers = NULL;
    }
}

int SaveLocalParam(void)
{
    FILE *fp = fopen("/mnt/img/.hqtds", "w+");
    if (fp == NULL)
    {
        DVR_DEBUG("open config file failed!");
        return -1;
    }
    int ret = fwrite(&allConfig.AllConfigDef, 1, sizeof(AllConfigDef_t), fp);
    fflush(fp);
    fsync(fp);
    fclose(fp);
    if (ret != sizeof(AllConfigDef_t))
    {
        DVR_DEBUG("write config file failed!");
        return -2;
    }
    // DVR_DEBUG("config file saved!");
    return 0;
}

int get_local_ip(char *ip, size_t iplen)
{
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;

    if (getifaddrs(&ifaddr) == -1)
    {
        perror("getifaddrs");
        return -1;
    }
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK))
        { 
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            strncpy(ip, inet_ntoa(addr->sin_addr), iplen - 1);
            ip[iplen - 1] = '\0'; 
            found = 1;
            break;
        }
    }
    freeifaddrs(ifaddr);
    return found ? 0 : -1;
}

extern void *alarm_snap_task(void *arg);

void *OverspeedMonitorThread(void *arg)
{
    DebugFeatureConfig_t debug_cfg;
    DeviceConfig_t deviceconfig;
    rk_get_debug_feature_config(&debug_cfg);
    rk_get_device_config(&deviceconfig);

    time_t last_pre_warning_time[4] = {0};      // 上一次“超速预警”语音提示,预警间隔;
    time_t last_alarm_warning_time[4] = {0};    // 上一次“超速报警”语音提示,超速报警提示的最小间隔
    time_t last_overspeed_time[4] = {0};        // 超速持续多长时间,持续完就触发超速报警
    static int last_alarm_state[4] = {0};       // 0=超速未报警，1=超速已报警
    static int last_pre_warning_state[4] = {0}; // 0=未超速预警，1=已超速预警
    time_t start_time = time(NULL);
    // int current_speed = 0;

    while (1)
    {
        time_t current_time = time(NULL);
        int current_speed = get_dms_current_speed();
        // DVR_DEBUG("current_speed = %d\n", current_speed);
        //  SSYFI_GPS m_gps;
        //  memset(&m_gps, 0, sizeof(SSYFI_GPS));
        //  GetBackBoardGpsData(&m_gps);
        //  int current_speed = m_gps.kiloSpeed;
        //  if (difftime(current_time, start_time) >= 10) {
        //      current_speed = 10;
        //      DVR_DEBUG("current speed updated: %d km/h", current_speed);
        //  }

        for (int i = 0; i < 4; i++)
        {
            int overspeed_duration = debug_cfg.speedWarning.channels[i].persistenceDuration;
            if (debug_cfg.speedWarning.channels[i].enabled && debug_cfg.speedWarning.channels[i].channel == (i + 1))
            {
                int pre_warn = debug_cfg.speedWarning.channels[i].preWarningSpeed;
                int alarm_warn = debug_cfg.speedWarning.channels[i].speedThreshold;
                if (last_alarm_state[i] == 1 && current_speed < alarm_warn) // 已经报警，速度降下来
                {
                    OverspeedInfo_t OverspeedInfo = {0};
                    OverspeedInfo.channel = i + 1;
                    OverspeedInfo.speed = current_speed;
                    OverspeedInfo.threshold = alarm_warn;
                    OverspeedInfo.timestamp = current_time;
                    OverspeedInfo.gpsStatus = 1;
                    CBuBiaoAPIDemo::Instance()->InputOverSpeed((char *)&OverspeedInfo, sizeof(OverspeedInfo_t), MDVR_ALARM_SPEED, 0, DVR_SPEED_HIGH_ALARM);
                    last_alarm_state[i] = 0;
                    last_overspeed_time[i] = 0;
                    DVR_DEBUG("channel%d overspeed alarm canceled! current speed:%d, threshold:%d", i + 1, current_speed, alarm_warn);
                }

                if (last_pre_warning_state[i] == 1 && (current_speed < pre_warn || current_speed >= alarm_warn))
                {
                    OverspeedInfo_t OverspeedInfo = {0};
                    OverspeedInfo.channel = i + 1;
                    OverspeedInfo.speed = current_speed;
                    OverspeedInfo.threshold = pre_warn;
                    OverspeedInfo.timestamp = current_time;
                    OverspeedInfo.gpsStatus = 1;
                    // 上报预警解除
                    CBuBiaoAPIDemo::Instance()->InputOverSpeed((char *)&OverspeedInfo, sizeof(OverspeedInfo_t), MDVR_ALARM_SPEED, 0, DVR_SPEED_HIGH_WARMING);
                    last_pre_warning_state[i] = 0;
                    DVR_DEBUG("channel%d pre-warning canceled! current speed:%d, threshold:%d", i + 1, current_speed, pre_warn);
                }

                if (current_speed < alarm_warn && current_speed >= pre_warn)
                {
                    // 首次进入预警区间，上报一次“预警”
                    if (last_pre_warning_state[i] == 0)
                    {
                        OverspeedInfo_t OverspeedInfo = {0};
                        OverspeedInfo.channel = i + 1;
                        OverspeedInfo.speed = current_speed;
                        OverspeedInfo.threshold = pre_warn;
                        OverspeedInfo.timestamp = current_time;
                        OverspeedInfo.gpsStatus = 1;
                        CBuBiaoAPIDemo::Instance()->InputOverSpeed((char *)&OverspeedInfo, sizeof(OverspeedInfo_t), MDVR_ALARM_SPEED, 1, DVR_SPEED_HIGH_WARMING);
                        last_pre_warning_state[i] = 1;
                        DVR_DEBUG("channel%d pre-warning triggered! current speed:%d, threshold:%d", i + 1, current_speed, pre_warn);
                    }

                    if (difftime(current_time, last_pre_warning_time[i]) >= debug_cfg.speedWarning.channels[i].preWarningInterval)
                    {
                        DVR_DEBUG("channel%d pre-warning! current speed:%d speed_threshold:%d", i + 1, current_speed, pre_warn);
                        PcmPlayer::Instance()->AddTTS("超速预警请注意减速");
                        last_pre_warning_time[i] = current_time;
                    }
                }

                if (current_speed >= alarm_warn && alarm_warn > 0)
                {
                    if (last_overspeed_time[i] == 0)
                    {
                        last_overspeed_time[i] = current_time;
                    }
                    if (difftime(current_time, last_overspeed_time[i]) >= overspeed_duration)
                    {
                        if (difftime(current_time, last_alarm_warning_time[i]) >= debug_cfg.speedWarning.channels[i].warningInterval)
                        {
                            DVR_DEBUG("channel%d is overspeed! current speed:%d speed_threshold:%d", i + 1, current_speed, alarm_warn);
                            PcmPlayer::Instance()->AddTTS("已超速请降低车速");
                            last_alarm_warning_time[i] = current_time;
                            OverspeedInfo_t OverspeedInfo = {0};
                            OverspeedInfo.channel = i + 1;
                            OverspeedInfo.speed = current_speed;
                            OverspeedInfo.threshold = alarm_warn;
                            OverspeedInfo.timestamp = current_time;
                            // OverspeedInfo.latitude = 39.916527;
                            // OverspeedInfo.longitude = 116.397128;
                            OverspeedInfo.gpsStatus = 1;
                            CBuBiaoAPIDemo::Instance()->InputOverSpeed((char *)&OverspeedInfo, sizeof(OverspeedInfo_t), MDVR_ALARM_SPEED, 1, DVR_SPEED_HIGH_ALARM);

                            // 抓拍功能
                            AlarmSnapTaskParam *taskParam = malloc(sizeof(AlarmSnapTaskParam));
                            if (!taskParam)
                            {
                                printf("[DMS][SNAP][ERROR] malloc failed for AlarmSnapTaskParam!\n");
                            }
                            else
                            {
                                taskParam->channel = i;
                                taskParam->picSize = PIC_D1;
                                taskParam->photoNumber = debug_cfg.speedWarning.channels[i].photoCount;
                                taskParam->photoIntervalMs = debug_cfg.speedWarning.channels[i].photoInterval * 1000;
                                DVR_DEBUG("taskParam->photoIntervalMs = %d\n", taskParam->photoIntervalMs);

                                pthread_t tid;
                                int th_ret = pthread_create(&tid, NULL, alarm_snap_task, taskParam);
                                if (th_ret != 0)
                                {
                                    free(taskParam);
                                }
                                else
                                {
                                    pthread_detach(tid);
                                }
                            }
                        }
                    }
                }
                else
                {
                }
            }
        }

        sleep(1);
    }
    return NULL;
}

/***********************************************************************************************************
**函数:超速线程
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
void CreatePthread_overspeed(void)
{
    pthread_t id;
    PTHREAD_PARAM_T *pParam;
    pParam = ComGetPthreadParamAddr(PTHREAD_OVERSPEED_ID);
    pParam->states = STATE_RUNNING;
    id = ComCreateThread(pParam, NULL, OverspeedMonitorThread);
    if (id == 0)
    {
        DVR_DEBUG("Create Led pthread fail");
        exit(-1);
    }
}

/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif


int rk_create_kit(const CreateKitRequest_t *request, CreateKitResponseData_t *response)
{
    if (!request || !response) return -1;

    memset(response, 0, sizeof(*response));

   response->brandId = 123;//default val
   PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.brandId,response->brandId);

    char fp[512];
    rk_build_fingerprint_from_request(request, fp, sizeof(fp));
    if (strcmp(fp, "imei=|mac=|sn=|vin=") == 0) {
        return -2;
    }

    if (rk_get_or_create_kitid(fp, response->kitID, sizeof(response->kitID)) != 0) {
        return -3;
    }

    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID,response->kitID);

    SAFE_STRCPY(response->email, request->kit.email);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.email,response->email);

    SAFE_STRCPY(response->phoneNumber, request->kit.phoneNumber);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.phoneNumber,response->phoneNumber);

    SAFE_STRCPY(response->licensePlate, request->kit.licensePlate);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.licensePlate,response->licensePlate);
    
    SAFE_STRCPY(response->vinNumber, request->kit.vinNumber);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.vinNumber,response->vinNumber);

    SAFE_STRCPY(response->imei, request->kit.imei);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.imei,response->imei);

    SAFE_STRCPY(response->model, request->kit.model);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.model,response->model);

    SAFE_STRCPY(response->serialNumber, request->kit.serialNumber);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.serialNumber,response->serialNumber);

    SAFE_STRCPY(response->ip, request->kit.ip);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.ip,response->ip);

    SAFE_STRCPY(response->mac, request->kit.mac);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.mac,response->mac);

    response->port = request->kit.port;
    PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.port, response->port);

    int dst_dev_cap = (int)(sizeof(response->devices) / sizeof(response->devices[0]));
    int dev_count   = request->kit.deviceCount;
    if (dev_count < 0) dev_count = 0;
    if (dev_count > dst_dev_cap) dev_count = dst_dev_cap;
    response->deviceCount = dev_count;
    PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.deviceCount, response->deviceCount);
    for (int i = 0; i < dev_count; ++i) {
        const CreateKitDeviceInfo_t *srcDev = &request->kit.devices[i];
        CreateKitDeviceInfo_t *dstDev       = &response->devices[i];

        SAFE_STRCPY(dstDev->name, srcDev->name);
        PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].name, response->devices[i].name);

        int dst_cam_cap = (int)(sizeof(dstDev->cams) / sizeof(dstDev->cams[0]));
        int cam_count   = srcDev->cameraCount;
        if (cam_count < 0) cam_count = 0;
        if (cam_count > dst_cam_cap) cam_count = dst_cam_cap;
        dstDev->cameraCount = cam_count;
        //allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cameraCount = cam_count;
        PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cameraCount, response->devices[i].cameraCount);
        for (int j = 0; j < cam_count; ++j) {
            const CreateKitCameraInfo_t *srcCam = &srcDev->cams[j];
            CreateKitCameraInfo_t *dstCam       = &dstDev->cams[j];

            dstCam->id = j + 1;
            dstCam->protocolType = srcCam->protocolType;
            dstCam->port         = srcCam->port;
            PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].id,dstCam->id);
            PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].protocolType,srcCam->protocolType);
            PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].port,srcCam->port);

            SAFE_STRCPY(dstCam->label, srcCam->label);
            PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].label,srcCam->label);

            SAFE_STRCPY(dstCam->endpoint, srcCam->endpoint);
            PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].endpoint,srcCam->endpoint);

            SAFE_STRCPY(dstCam->username, srcCam->username);
            PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].username,srcCam->username);

            SAFE_STRCPY(dstCam->password, srcCam->password);
            PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].password,srcCam->password);
        }

        int dst_btn_cap = (int)(sizeof(dstDev->buttons) / sizeof(dstDev->buttons[0]));
        int btn_count   = srcDev->buttonCount;
        if (btn_count < 0) btn_count = 0;
        if (btn_count > dst_btn_cap) btn_count = dst_btn_cap;
        dstDev->buttonCount = btn_count;
        PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttonCount,srcDev->buttonCount);

        for (int k = 0; k < btn_count; ++k) {
            const CreateKitButtonInfo_t *srcBtn = &srcDev->buttons[k];
            CreateKitButtonInfo_t *dstBtn       = &dstDev->buttons[k];

            dstBtn->pinNumber = srcBtn->pinNumber;
            dstBtn->type      = srcBtn->type;
            PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttons[k].pinNumber,srcBtn->pinNumber);
            PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttons[k].type,srcBtn->type);
            SAFE_STRCPY(dstBtn->serialNumber, srcBtn->serialNumber);
            PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttons[k].serialNumber,srcBtn->serialNumber);
        }
    }

    response->valid = true;
    PERSIST_BOOL(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.valid, response->valid);
    int rc = SaveParam();
    if (rc < 0) {
    char buf[CFG_LOG_BUFSZ];
    std::snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", rc);
    }
    return 0;
}

int rk_get_incident_images(const IncidentImageRequest_t *request, IncidentImageResponse_t *response)
{
    if (!request || !response) return -1;
    memset(response, 0, sizeof(*response));

    char reqKid[128];
    SAFE_STRCPY(reqKid, request->kitID);
    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;
    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);
    if (!kitid_validate_then_fill(reqKid, storedKid)) {
        response->isValid = 0;
        SAFE_STRCPY(response->error, "kitID is not match");
        return 0;
    }

    SAFE_STRCPY(response->eventID, request->eventID);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.incidentimagerequest.eventID, request->eventID);
    SAFE_STRCPY(response->kitID, storedKid);
    response->isValid = 1;
    response->error   = NULL;

    char *ids = allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.incidentImageIds;

    int chs[8] = {0};
    int chs_cnt = 0;
    build_channels_from_ids(ids, chs, &chs_cnt, 8);

    SnapResult *results = NULL;
    int rc = Doordvr_Snap_Multi_Base64(chs, chs_cnt, PIC_D1, &results);
    if (rc != 0 || !results) {
        DVR_DEBUG("Doordvr_Snap_Multi_Base64 failed, rc=%d", rc);
        response->isValid = 0;
        SAFE_STRCPY(response->error, "snap to base64 failed");
        return 0;
    }

    const size_t MIN_B64_LEN = 10000;

    char errbuf[256]; errbuf[0] = '\0';
    size_t eoff = 0;
    int bad_cnt = 0;

    int success_idx[8] = {0};
    int success_cnt = 0;
    for (int i = 0; i < chs_cnt; ++i) {
        bool ok = (results[i].b64 && (size_t)results[i].b64_len >= MIN_B64_LEN);
        if (ok) {
            success_idx[success_cnt++] = i;
        } else {
            int human_ch = chs[i]; 
            DVR_DEBUG("Snap ch_idx=%d (req_ch=%d) skipped: b64_len=%zu < %zu or null",
                      i, chs[i], (size_t)results[i].b64_len, MIN_B64_LEN);

            int n = snprintf(errbuf + eoff,
                             (eoff < sizeof(errbuf)) ? (sizeof(errbuf) - eoff) : 0,
                             (bad_cnt == 0) ? "%d路摄像头异常" : ",%d路摄像头异常",
                             human_ch);
            if (n > 0) {
                eoff += (size_t)n;
                if (eoff >= sizeof(errbuf)) eoff = sizeof(errbuf) - 1;
                errbuf[eoff] = '\0';
            }
            bad_cnt++;

            if (results[i].b64) { free(results[i].b64); results[i].b64 = NULL; }
        }
    }

    if (bad_cnt > 0) {
        SAFE_STRCPY(response->error, errbuf);
    }

    if (success_cnt == 0) {
        free(results);
        response->imageCount = 0;
        response->isValid = 0;
        if (bad_cnt == 0) SAFE_STRCPY(response->error, "no frame after length filter (<10000)");
        DVR_DEBUG("No valid frames (all below %zu). Early return. err='%s'",
                  MIN_B64_LEN, response->error ? response->error : "(null)");
        return 0;
    }

    response->imageCount = success_cnt;
    response->images = (IncidentImageInfo_t *)calloc((size_t)success_cnt, sizeof(*response->images));
    if (!response->images) {
        for (int j = 0; j < success_cnt; ++j) {
            int idx = success_idx[j];
            if (results[idx].b64) free(results[idx].b64);
        }
        free(results);
        response->imageCount = 0;
        response->isValid = 0;
        SAFE_STRCPY(response->error, "alloc images failed");
        return 0;
    }

    for (int k = 0; k < success_cnt; ++k) {
        int i = success_idx[k];
        response->images[k].id       = results[i].channel;   
        response->images[k].file     = results[i].b64;       
        response->images[k].fileSize = (int)results[i].b64_len;
        results[i].b64 = NULL;
    }

    for (int i = 0; i < chs_cnt; ++i) {
        if (results[i].b64) { free(results[i].b64); results[i].b64 = NULL; }
    }
    free(results);

    for (int i = 0; i < success_cnt; ++i) {
        DVR_DEBUG("Image[%d]: id=%d b64_len=%d", i+1, response->images[i].id, response->images[i].fileSize);
    }

    char ts[32];
    { time_t now = time(NULL); struct tm tm_info; localtime_r(&now, &tm_info);
      strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info); }

    bool any_save_failed = false;
    for (int i = 0; i < success_cnt; ++i) {
        char out_path[256];
        snprintf(out_path, sizeof(out_path), "/opt/C807RK/snap_ch%d_%s.jpg", response->images[i].id, ts);
        int src = save_base64_to_file(response->images[i].file, out_path);
        if (src == 0) {
            DVR_DEBUG("Saved decoded image: ch=%d path='%s' size(b64)=%d",
                      response->images[i].id, out_path, response->images[i].fileSize);
        } else {
            any_save_failed = true;
            DVR_DEBUG("save_base64_to_file failed: ch=%d path='%s' err=%d",
                      response->images[i].id, out_path, src);
        }
    }

    if (any_save_failed) {
        response->isValid = 0;
        SAFE_STRCPY(response->error, "save decoded image failed");
    }

    int z = SaveParam();
    if (z < 0) {
        char buf[CFG_LOG_BUFSZ];
        std::snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", z);
        DVR_DEBUG("%s", buf);
    }
    return 0;
}


int rk_get_device_info(const DeviceInfoRequest_t *request, DeviceInfoResponse_t *response) {
    if (!request || !response) return -1;

    memset(response, 0, sizeof(*response));

    char reqKid[sizeof(request->kitID)];
    SAFE_STRCPY(reqKid, request->kitID);

    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;

    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);

    if (!kitid_validate_then_fill(reqKid, storedKid)) {
        response->isValid = 0;
        SAFE_STRCPY(response->error, "kitID is not match");
        return 0;
    }

    DVR_DEBUG("GetDeviceInfo: kitID match -> return virtual data");

    SAFE_STRCPY(response->kitID, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID);

    SSYFI_GPS g_curGpsInfo;
    memset(&g_curGpsInfo, 0, sizeof(SSYFI_GPS));
    GetBackBoardGpsData(&g_curGpsInfo);

    SAFE_STRCPY(response->kitInfoDto.IMEI, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.imei);
    SAFE_STRCPY(response->kitInfoDto.plateNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.plateNumber);
    SAFE_STRCPY(response->kitInfoDto.chasisNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.chasisNumber);
    SAFE_STRCPY(response->kitInfoDto.version, "default");//virture value
    SAFE_STRCPY(response->kitInfoDto.ip, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.ip);
    response->kitInfoDto.port = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.port;
    SAFE_STRCPY(response->kitInfoDto.model, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.model);
    response->kitInfoDto.brand = 123;//virture vale
    DeviceLocationInfo_t *loc = &response->kitInfoDto.locationInfoDto;

    memset(loc, 0, sizeof(*loc)); 

    double lat_deg = 0.0, lon_deg = 0.0;
    int    r95_m   = 0;
    char   err_type[64]  = {0};
    char   err_desc[128] = {0};

    int ok = parse_gps_to_deg(&g_curGpsInfo, &lat_deg, &lon_deg, &r95_m,
                              err_type, err_desc, sizeof(err_desc));
    if (ok) {
        snprintf(loc->latitude,  sizeof(loc->latitude),  "%.7f", lat_deg);
        snprintf(loc->longitude, sizeof(loc->longitude), "%.7f", lon_deg);
        loc->radius = r95_m;
    } else {
        loc->latitude[0]  = '\0';
        loc->longitude[0] = '\0';
        loc->radius       = 0;
    }

    SAFE_STRCPY(loc->errorType,        err_type);
    SAFE_STRCPY(loc->errorDescription, err_desc);

    SAFE_STRCPY(loc->serialNumber, response->kitInfoDto.IMEI);

    DVR_DEBUG("GPS parsed -> lat=%s, lon=%s, r95=%d, errType='%s', errDesc='%s'",loc->latitude, loc->longitude, loc->radius,loc->errorType, loc->errorDescription);

    SAFE_STRCPY(loc->serialNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].buttons[0].serialNumber);
    response->kitInfoDto.cameraCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cameraCount;
    for (int i = 0; i < response->kitInfoDto.cameraCount; ++i) {
    response->kitInfoDto.cameraInfoDto[i].Id = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].id;
    response->kitInfoDto.cameraInfoDto[i].type = 1;//default value
    response->kitInfoDto.cameraInfoDto[i].port = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].port;
    response->kitInfoDto.cameraInfoDto[i].protocolType = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].protocolType;
    SAFE_STRCPY(response->kitInfoDto.cameraInfoDto[i].label, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].label);
    SAFE_STRCPY(response->kitInfoDto.cameraInfoDto[i].streamEndpoint, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].endpoint);
    SAFE_STRCPY(response->kitInfoDto.cameraInfoDto[i].username, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].username);
    SAFE_STRCPY(response->kitInfoDto.cameraInfoDto[i].password, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].password);
    response->kitInfoDto.cameraInfoDto[i].errorType[0] = '\0';
    response->kitInfoDto.cameraInfoDto[i].errorDescription[0] = '\0';
    }
    SAFE_STRCPY(response->error, "null");
    response->isValid = 1;
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.latitude, loc->latitude);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.longitude, loc->longitude);
    PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.radius, loc->radius);
    int rc = SaveParam();
    if (rc < 0) {
    char buf[CFG_LOG_BUFSZ];
    std::snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", rc);
    }
    return 0;
}

int rk_get_current_location(const CurrentLocationRequest_t *request, CurrentLocationResponse_t *response)
{
    if (!request || !response) return -1;

    memset(response, 0, sizeof(*response));

    // kitID 校验保持原样
    char reqKid[sizeof(request->kitID)];
    SAFE_STRCPY(reqKid, request->kitID);
    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;

    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);

    if (!kitid_validate_then_fill(reqKid, storedKid)) {
        DVR_DEBUG("not right kitID");
        response->isValid = false;
        return 0;
    }

    DVR_DEBUG("GetDeviceInfo: kitID match -> return virtual data");
    LocationData_t *loc = &response->locationData;
    memset(loc, 0, sizeof(*loc));

    SAFE_STRCPY(loc->plateNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.plateNumber);
    SAFE_STRCPY(loc->kitID,       request->kitID);

    double lat_deg = 0.0, lon_deg = 0.0;
    int    r95_m   = 0;
    int    direction_deg = 0;
    char   err_type[64]  = {0};
    char   err_desc[128] = {0};

    int ok = get_current_location_metrics(&lat_deg, &lon_deg, &r95_m, &direction_deg,
                                          err_type, err_desc, sizeof(err_desc));

    DVR_DEBUG("parse_gps_to_deg: ok=%d, errType='%s', errDesc='%s', lat=%.7f, lon=%.7f, r95=%d",
              ok, err_type, err_desc, lat_deg, lon_deg, r95_m);

    if (ok) {
        snprintf(loc->latitude,  sizeof(loc->latitude),  "%.7f", lat_deg);
        snprintf(loc->longitude, sizeof(loc->longitude), "%.7f", lon_deg);
        loc->radius    = r95_m;
        loc->direction = direction_deg;
    } else {
        loc->latitude[0]  = '\0';
        loc->longitude[0] = '\0';
        loc->radius       = 0;
        loc->direction    = direction_deg; 
        DVR_DEBUG("parse_gps_to_deg failed: %s / %s", err_type, err_desc);
    }

    int dir = (int)loc->direction;
    dir %= 360;
    if (dir < 0) dir += 360;

    static const char* compass16[16] = {
        "N","NNE","NE","ENE","E","ESE","SE","SSE",
        "S","SSW","SW","WSW","W","WNW","NW","NNW"
    };
    int idx = (int)lround(dir / 22.5) & 15;
    DVR_DEBUG("Direction -> %d° (%s)  [0=N, 90=E, 180=S, 270=W]", dir, compass16[idx]);

    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.latitude,  loc->latitude);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.longitude, loc->longitude);
    PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.radius,    loc->radius);
    if(loc->latitude && loc->longitude && loc->radius)
    response->isValid = true;
    else
    response->isValid = false;
    response->error[0] = '\0';

    int rc = SaveParam();
    if (rc < 0) {
        char buf[CFG_LOG_BUFSZ];
        std::snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", rc);
        // 这里保持你原代码的语义：只是构造日志字符串，不改变返回值/流程
    }
    return 0;
}

int rk_get_health_check(const HealthCheckRequest_t *request, HealthCheckResponse_t *response)
{
    if (!request || !response) return -1;

    memset(response, 0, sizeof(*response));

    SSYFI_GPS g_curGpsInfo;
    memset(&g_curGpsInfo, 0, sizeof(SSYFI_GPS));
    GetBackBoardGpsData(&g_curGpsInfo);

    char reqKid[sizeof(request->kitID)];
    SAFE_STRCPY(reqKid, request->kitID);
    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;

    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);

    if (!kitid_validate_then_fill(reqKid, storedKid)) {
        DVR_DEBUG("not right kitID");
        SAFE_STRCPY(response->healthCheckData.kitInfoDto.errorType, "kitID is not match");
        response->isValid = false;
        return 0;
    }

    DVR_DEBUG("GetDeviceInfo: kitID match -> return virtual data");
    SAFE_STRCPY(response->healthCheckData.kitID, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID);
    SAFE_STRCPY(response->healthCheckData.kitInfoDto.IMEI, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.imei);
    SAFE_STRCPY(response->healthCheckData.kitInfoDto.plateNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.plateNumber);
    SAFE_STRCPY(response->healthCheckData.kitInfoDto.errorType, '\0');
    SAFE_STRCPY(response->healthCheckData.kitInfoDto.errorDescription, '\0');
    response->healthCheckData.cameraCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cameraCount;
    DVR_DEBUG("cam count = %d\n", response->healthCheckData.cameraCount);
    for(int i = 0; i < response->healthCheckData.cameraCount; ++i){
        SAFE_STRCPY(response->healthCheckData.cameraInfoDto[i].label, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[i].label);
        response->healthCheckData.cameraInfoDto[i].type = 1;//default val
        SAFE_STRCPY(response->healthCheckData.cameraInfoDto[i].errorType, '\0');//default val
        SAFE_STRCPY(response->healthCheckData.cameraInfoDto[i].errorDescription, '\0');//default val
    }
    fill_location_status_from_gps(
        &g_curGpsInfo,
        response->healthCheckData.locationInfoDto.status,
        sizeof(response->healthCheckData.locationInfoDto.status)
    );

  response->isValid = true;
  response->error[0] = '\0';

  return 0;
}

int rk_config_device_settings(const ConfigDeviceSettingsRequest_t *request, ConfigDeviceSettingsResponse_t *response)
{
    if (!request || !response) return -1;

    memset(response, 0, sizeof(*response));

    char reqKid[sizeof(request->kitID)];
    SAFE_STRCPY(reqKid, request->kitID);
    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;

    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);

    if (!kitid_validate_then_fill(reqKid, storedKid) || !request->faceRecognition) {
        DVR_DEBUG("not right kitID or not faceRecognition");
        response->isValid = false;
        return 0;
    }

    DVR_DEBUG("GetDeviceInfo: kitID match -> return virtual data");
  
    SAFE_STRCPY(response->parsedApplied.data.kitID, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID);
    response->isValid = true;
    SAFE_STRCPY(response->error, '\0');//default val
    SAFE_STRCPY(response->parsedApplied.data.plateNumber, request->plateNumber);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.plateNumber,response->parsedApplied.data.plateNumber);
    SAFE_STRCPY(response->parsedApplied.data.chasisNumber, request->chasisNumber);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.chasisNumber,response->parsedApplied.data.chasisNumber);
    response->parsedApplied.data.faceRecognition = request->faceRecognition;
    //PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.faceRecognition, response->parsedApplied.data.faceRecognition);

    SAFE_STRCPY(response->parsedApplied.message, "Settings Applied, Device Restarting...");//default val
    response->parsedApplied.success = true;
    response->parsedApplied.status    = 200; //default val                    
    response->parsedApplied.timestamp = (long)time(NULL);
    int rc = SaveParam();
    if (rc < 0) {
    char buf[CFG_LOG_BUFSZ];
    std::snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", rc);
    }
    return 0;
}

int rk_create_incident(const CreateIncidentRequest_t *request, CreateIncidentResponse_t *response)
{
    if (!request || !response) return -1;
    memset(response, 0, sizeof(*response));

    const int MAX_IMAGE_IDS_LIMIT = 4096;

    char reqKid[sizeof(request->kitID)];
    SAFE_STRCPY(reqKid, request->kitID);
    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;

    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);

    int kid_ok    = kitid_validate_then_fill(reqKid, storedKid);
    int lat_ok    = kitid_validate_then_fill(request->kitInfoDto.locationInfoDto.latitude,
                                         allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.latitude);
    int lon_ok    = kitid_validate_then_fill(request->kitInfoDto.locationInfoDto.longitude,
                                         allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.longitude);
    int radius_ok = (request->kitInfoDto.locationInfoDto.radius == allConfig.AllConfigDef.abdparam.IdaCfgSet.currentlocationresponse.locationData.radius);

    int all_ok = (kid_ok && lat_ok && lon_ok && radius_ok);

    if (!kid_ok)    DVR_DEBUG("not right kitID");
    if (!lat_ok)    DVR_DEBUG("not right latitude");
    if (!lon_ok)    DVR_DEBUG("not right longitude");
    if (!radius_ok) DVR_DEBUG("not right radius");

    int req_cam_n = request->kitInfoDto.cameraCount; 
    int cfg_cam_n = (req_cam_n < allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cameraCount) ? req_cam_n : allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cameraCount;

    if (req_cam_n != cfg_cam_n) {
        DVR_DEBUG("cameraCount mismatch: req=%d, cfg=%d", req_cam_n, cfg_cam_n);
        all_ok = 0;
    }

    int req_img_n = request->imageIdCount;
    if (req_img_n < 0) req_img_n = 0;
    if (req_img_n > MAX_IMAGE_IDS_LIMIT) req_img_n = MAX_IMAGE_IDS_LIMIT;

    if (cfg_cam_n != req_img_n) {
        DVR_DEBUG("count mismatch: cameraCount(cfg)=%d, imageIdCount(req)=%d", cfg_cam_n, req_img_n);
        all_ok = 0;
    }

    if (req_cam_n != req_img_n) {
        DVR_DEBUG("request internal mismatch: cameraCount(req)=%d, imageIdCount(req)=%d", req_cam_n, req_img_n);
        all_ok = 0;
    }
    int n = (req_cam_n < cfg_cam_n) ? req_cam_n : cfg_cam_n;

for (int t = 0; t < n; ++t) {
    const typeof(request->kitInfoDto.cameraInfoDto[0]) *reqCam = &request->kitInfoDto.cameraInfoDto[t];
    const typeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[0]) *cfgCam = &allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[0].cams[t];

    int label_ok_t    = kitid_validate_then_fill(reqCam->label,          cfgCam->label);
    int port_ok_t     = (reqCam->port == cfgCam->port); 
    int endpoint_ok_t = kitid_validate_then_fill(reqCam->streamEndpoint, cfgCam->endpoint);
    int username_ok_t = kitid_validate_then_fill(reqCam->username,       cfgCam->username);
    int password_ok_t = kitid_validate_then_fill(reqCam->password,       cfgCam->password);

    if (!label_ok_t)    { DVR_DEBUG("cam[%d] not right label", t);       all_ok = 0; }
    if (!port_ok_t)     { DVR_DEBUG("cam[%d] not right port", t);        all_ok = 0; }
    if (!endpoint_ok_t) { DVR_DEBUG("cam[%d] not right endpoint", t);    all_ok = 0; }
    if (!username_ok_t) { DVR_DEBUG("cam[%d] not right username", t);    all_ok = 0; }
    if (!password_ok_t) { DVR_DEBUG("cam[%d] not right password", t);    all_ok = 0; }
    }

    if (!all_ok) {
        response->isValid = 0;
        return 0;
    }
    DVR_DEBUG("match successfully\n");
    SAFE_STRCPY(response->kitID, request->kitID);
    SAFE_STRCPY(response->eventID, request->eventID);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.eventID, response->eventID);
    SAFE_CPYI64(response->caseStartedDateTime, request->caseStartedDateTime);
    PERSIST_I64(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.caseStartedDateTime, response->caseStartedDateTime);

    response->imageIdCount = request->imageIdCount;
    if (response->imageIdCount < 0) {
        DVR_DEBUG("invalid imageIdCount=%d, clamp to 0", response->imageIdCount);
        response->imageIdCount = 0;
    }
    if (response->imageIdCount > MAX_IMAGE_IDS_LIMIT) {
        DVR_DEBUG("imageIdCount too large (%d), clamp to %d",
                  response->imageIdCount, MAX_IMAGE_IDS_LIMIT);
        response->imageIdCount = MAX_IMAGE_IDS_LIMIT;
    }
    PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.imageIdCount,
                response->imageIdCount);

    if (response->imageIdCount > 0 && !request->incidentImageIds) {
        DVR_DEBUG("request->incidentImageIds is NULL while count=%d", response->imageIdCount);
        return -2;
    }

    if (response->imageIdCount > 0) {
        response->incidentImageIds = (int*)calloc((size_t)response->imageIdCount, sizeof(int));
        if (!response->incidentImageIds) {
            DVR_DEBUG("calloc response->incidentImageIds failed, count=%d", response->imageIdCount);
            return -3;
        }
        memcpy(response->incidentImageIds,
               request->incidentImageIds,
               sizeof(int) * (size_t)response->imageIdCount);
    } else {
        response->incidentImageIds = NULL;
    }

    CreateIncidentResponse *cfg = &allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse;

    char csvBuf[sizeof(cfg->incidentImageIds)];
    if (response->imageIdCount > 0 && response->incidentImageIds) {
        join_image_ids_csv(response->incidentImageIds,
                           response->imageIdCount,
                           csvBuf,
                           sizeof(csvBuf));
        PERSIST_STR(cfg->incidentImageIds, csvBuf);
    } else {
        PERSIST_STR(cfg->incidentImageIds, "");
    }

    for (int i = 0; i < response->imageIdCount; ++i) {
        DVR_DEBUG("imgId[%d]: req=%d, resp=%d",
                  i,
                  request->incidentImageIds[i],
                  response->incidentImageIds[i]);
    }
    DVR_DEBUG("cfg incidentImageIds(csv)='%s'", cfg->incidentImageIds);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.incidentImageIds, cfg->incidentImageIds);
    DVR_DEBUG("imageID = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.incidentImageIds);
    CreateIncidentKitInfo_t *k = &response->kitInfoDto;
    memset(k, 0, sizeof(*k));
    SAFE_STRCPY(response->kitInfoDto.locationInfoDto.latitude, request->kitInfoDto.locationInfoDto.latitude);
    SAFE_STRCPY(response->kitInfoDto.locationInfoDto.longitude, request->kitInfoDto.locationInfoDto.longitude);
    response->kitInfoDto.locationInfoDto.radius = request->kitInfoDto.locationInfoDto.radius; 
    SAFE_STRCPY(k->IMEI, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.imei);
    //PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.IMEI, request->kitInfoDto.IMEI);
    SAFE_STRCPY(k->plateNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.configdevicesettingsresponse.parsedApplied.data.plateNumber);
    //PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.plateNumber, request->kitInfoDto.plateNumber);
    SAFE_STRCPY(k->ip, request->kitInfoDto.ip);
    PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.ip, request->kitInfoDto.ip);
    k->port = request->kitInfoDto.port;
    PERSIST_INT(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.port, request->kitInfoDto.port);

    k->cameraCount = request->kitInfoDto.cameraCount;
    for(int t = 0; t < request->kitInfoDto.cameraCount; ++t){
        k->cameraInfoDto[t].type = request->kitInfoDto.cameraInfoDto[t].type;
        k->cameraInfoDto[t].port = request->kitInfoDto.cameraInfoDto[t].port;
        SAFE_STRCPY(k->cameraInfoDto[t].label, request->kitInfoDto.cameraInfoDto[t].label);
        //PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.cameraInfoDto[t].label, k->cameraInfoDto[t].label);
        SAFE_STRCPY(k->cameraInfoDto[t].streamEndpoint, request->kitInfoDto.cameraInfoDto[t].streamEndpoint);
       //PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.cameraInfoDto[t].streamEndpoint, k->cameraInfoDto[t].streamEndpoint);
        SAFE_STRCPY(k->cameraInfoDto[t].username, request->kitInfoDto.cameraInfoDto[t].username);
        //PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.cameraInfoDto[t].username, k->cameraInfoDto[t].username);
        SAFE_STRCPY(k->cameraInfoDto[t].password, request->kitInfoDto.cameraInfoDto[t].password);
        //PERSIST_STR(allConfig.AllConfigDef.abdparam.IdaCfgSet.createincidentresponse.kitInfoDto.cameraInfoDto[t].password, k->cameraInfoDto[t].password);
        k->cameraInfoDto[t].errorType[0]        = '\0';
        k->cameraInfoDto[t].errorDescription[0] = '\0';
    }
    k->errorType[0]        = '\0';
    k->errorDescription[0] = '\0';

    response->isValid = true;

    int rc = SaveParam();
    if (rc < 0) {
    char buf[CFG_LOG_BUFSZ];
    std::snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", rc);
    }

    return 0;
}


    
// 检查摄像头
int rk_check_cameras(const CameraCheckRequest_t *request, CameraCheckResponse_t* response){
    if (!response) return -1;

    memset(response, 0, sizeof(*response));

    char reqKid[sizeof(request->kitID)];
    SAFE_STRCPY(reqKid, request->kitID);
    const char *storedKid = allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID;

    DVR_DEBUG("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);

    if (!kitid_validate_then_fill(reqKid, storedKid)) {
        DVR_DEBUG("not right kitID");
        response->isValid = false;
        return 0;
    }

    SAFE_STRCPY(response->kitID, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID);
    response->isValid = true;

    response->cameraCount = request->cameraCount;

    for (int i = 0; i < response->cameraCount; ++i) {
        response->camsTestInfo[i].id      = request->camsTestInfo[i].id;        
        response->camsTestInfo[i].isValid = true;  
        snprintf(response->camsTestInfo[i].desc,
                 sizeof(response->camsTestInfo[i].desc),
                 request->camsTestInfo[i].desc);
    }


    return 0;
}


    int rk_adas_get_preview_config(ADASPreviewConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        memset(cfg, 0, sizeof(*cfg));
        cfg->horizon = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.horizon;
        cfg->vehicleCenterLine = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.carMiddle;
        cfg->vehicleWidth = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.carWidth;
        cfg->cameraHeight = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.cameraHeight;
        cfg->distanceCamera2FrontWheel = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.cameraToAxle;
        cfg->distanceCamera2Front = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.cameraToBumper;
        return 0;
    }

    int rk_adas_set_preview_config(ADASPreviewConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.horizon = cfg->horizon;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.carMiddle = cfg->vehicleCenterLine;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.carWidth = cfg->vehicleWidth;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.cameraHeight = cfg->cameraHeight;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.cameraToAxle = cfg->distanceCamera2FrontWheel;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.CalibInfo.cameraToBumper = cfg->distanceCamera2Front;
        SaveLocalParam();
        return 0;
    }

    int rk_dms_get_config(DMSConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        memset(cfg, 0, sizeof(*cfg));
        cfg->warningParams[0].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].enabled;
        cfg->warningParams[0].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].alarmInterval;
        cfg->warningParams[0].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].photoNumber;
        cfg->warningParams[0].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].sensitivity;
        cfg->warningParams[0].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].recordVideoSeconds;
        cfg->warningParams[0].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].speedRequirement;
        cfg->warningParams[0].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].photoInterval;
        cfg->warningParams[0].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].firstLevelAlarmSpeed;
        cfg->warningParams[0].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].secondLevelAlarmSpeed;
        cfg->warningParams[1].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].enabled;
        cfg->warningParams[1].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].alarmInterval;
        cfg->warningParams[1].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].photoNumber;
        cfg->warningParams[1].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].sensitivity;
        cfg->warningParams[1].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].recordVideoSeconds;
        cfg->warningParams[1].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].speedRequirement;
        cfg->warningParams[1].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].photoInterval;
        cfg->warningParams[1].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].firstLevelAlarmSpeed;
        cfg->warningParams[1].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].secondLevelAlarmSpeed;
        cfg->warningParams[2].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].enabled;
        cfg->warningParams[2].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].alarmInterval;
        cfg->warningParams[2].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].photoNumber;
        cfg->warningParams[2].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].sensitivity;
        cfg->warningParams[2].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].recordVideoSeconds;
        cfg->warningParams[2].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].speedRequirement;
        cfg->warningParams[2].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].photoInterval;
        cfg->warningParams[2].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].firstLevelAlarmSpeed;
        cfg->warningParams[2].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].secondLevelAlarmSpeed;
        cfg->warningParams[3].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].enabled;
        cfg->warningParams[3].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].alarmInterval;
        cfg->warningParams[3].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].photoNumber;
        cfg->warningParams[3].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].sensitivity;
        cfg->warningParams[3].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].recordVideoSeconds;
        cfg->warningParams[3].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].speedRequirement;
        cfg->warningParams[3].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].photoInterval;
        cfg->warningParams[3].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].firstLevelAlarmSpeed;
        cfg->warningParams[3].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].secondLevelAlarmSpeed;
        cfg->warningParams[4].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].enabled;
        cfg->warningParams[4].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].alarmInterval;
        cfg->warningParams[4].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].photoNumber;
        cfg->warningParams[4].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].sensitivity;
        cfg->warningParams[4].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].recordVideoSeconds;
        cfg->warningParams[4].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].speedRequirement;
        cfg->warningParams[4].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].photoInterval;
        cfg->warningParams[4].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].firstLevelAlarmSpeed;
        cfg->warningParams[4].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].secondLevelAlarmSpeed;
        cfg->warningParams[5].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].enabled;
        cfg->warningParams[5].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].alarmInterval;
        cfg->warningParams[5].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].photoNumber;
        cfg->warningParams[5].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].sensitivity;
        cfg->warningParams[5].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].recordVideoSeconds;
        cfg->warningParams[5].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].speedRequirement;
        cfg->warningParams[5].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].photoInterval;
        cfg->warningParams[5].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].firstLevelAlarmSpeed;
        cfg->warningParams[5].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].secondLevelAlarmSpeed;
        cfg->warningParams[6].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].enabled;
        cfg->warningParams[6].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].alarmInterval;
        cfg->warningParams[6].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].photoNumber;
        cfg->warningParams[6].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].sensitivity;
        cfg->warningParams[6].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].recordVideoSeconds;
        cfg->warningParams[6].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].speedRequirement;
        cfg->warningParams[6].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].photoInterval;
        cfg->warningParams[6].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].firstLevelAlarmSpeed;
        cfg->warningParams[6].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].secondLevelAlarmSpeed;
        cfg->alarmVolume = allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.alarmVolume;
        return 0;
    }

    int rk_dms_set_config(DMSConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].enabled = cfg->warningParams[0].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].alarmInterval = cfg->warningParams[0].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].photoNumber = cfg->warningParams[0].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].sensitivity = cfg->warningParams[0].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].recordVideoSeconds = cfg->warningParams[0].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].speedRequirement = cfg->warningParams[0].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].photoInterval = cfg->warningParams[0].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].firstLevelAlarmSpeed = cfg->warningParams[0].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[0].secondLevelAlarmSpeed = cfg->warningParams[0].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].enabled = cfg->warningParams[1].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].alarmInterval = cfg->warningParams[1].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].photoNumber = cfg->warningParams[1].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].sensitivity = cfg->warningParams[1].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].recordVideoSeconds = cfg->warningParams[1].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].speedRequirement = cfg->warningParams[1].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].photoInterval = cfg->warningParams[1].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].firstLevelAlarmSpeed = cfg->warningParams[1].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[1].secondLevelAlarmSpeed = cfg->warningParams[1].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].enabled = cfg->warningParams[2].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].alarmInterval = cfg->warningParams[2].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].photoNumber = cfg->warningParams[2].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].sensitivity = cfg->warningParams[2].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].recordVideoSeconds = cfg->warningParams[2].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].speedRequirement = cfg->warningParams[2].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].photoInterval = cfg->warningParams[2].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].firstLevelAlarmSpeed = cfg->warningParams[2].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[2].secondLevelAlarmSpeed = cfg->warningParams[2].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].enabled = cfg->warningParams[3].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].alarmInterval = cfg->warningParams[3].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].photoNumber = cfg->warningParams[3].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].sensitivity = cfg->warningParams[3].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].recordVideoSeconds = cfg->warningParams[3].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].speedRequirement = cfg->warningParams[3].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].photoInterval = cfg->warningParams[3].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].firstLevelAlarmSpeed = cfg->warningParams[3].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[3].secondLevelAlarmSpeed = cfg->warningParams[3].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].enabled = cfg->warningParams[4].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].alarmInterval = cfg->warningParams[4].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].photoNumber = cfg->warningParams[4].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].sensitivity = cfg->warningParams[4].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].recordVideoSeconds = cfg->warningParams[4].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].speedRequirement = cfg->warningParams[4].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].photoInterval = cfg->warningParams[4].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].firstLevelAlarmSpeed = cfg->warningParams[4].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[4].secondLevelAlarmSpeed = cfg->warningParams[4].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].enabled = cfg->warningParams[5].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].alarmInterval = cfg->warningParams[5].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].photoNumber = cfg->warningParams[5].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].sensitivity = cfg->warningParams[5].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].recordVideoSeconds = cfg->warningParams[5].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].speedRequirement = cfg->warningParams[5].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].photoInterval = cfg->warningParams[5].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].firstLevelAlarmSpeed = cfg->warningParams[5].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[5].secondLevelAlarmSpeed = cfg->warningParams[5].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].enabled = cfg->warningParams[6].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].alarmInterval = cfg->warningParams[6].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].photoNumber = cfg->warningParams[6].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].sensitivity = cfg->warningParams[6].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].recordVideoSeconds = cfg->warningParams[6].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].speedRequirement = cfg->warningParams[6].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].photoInterval = cfg->warningParams[6].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].firstLevelAlarmSpeed = cfg->warningParams[6].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.warningParam[6].secondLevelAlarmSpeed = cfg->warningParams[6].secondLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.alarmVolume = cfg->alarmVolume;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.systemVolume = cfg->alarmVolume;
        SaveLocalParam();
        return 0;
    }

    int rk_adas_get_config(ADASConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        memset(cfg, 0, sizeof(*cfg));
        pthread_mutex_lock(&allConfig.config_lock);
        cfg->warningParams[0].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].enabled;
        cfg->warningParams[0].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].alarmInterval;
        cfg->warningParams[0].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].photoNumber;
        cfg->warningParams[0].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].sensitivity;
        cfg->warningParams[0].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].recordVideoSeconds;
        cfg->warningParams[0].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].speedRequirement;
        cfg->warningParams[0].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].photoInterval;
        cfg->warningParams[0].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].firstLevelAlarmSpeed;
        cfg->warningParams[0].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].secondLevelAlarmSpeed;
        cfg->warningParams[1].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].enabled;
        cfg->warningParams[1].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].alarmInterval;
        cfg->warningParams[1].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].photoNumber;
        cfg->warningParams[1].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].sensitivity;
        cfg->warningParams[1].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].recordVideoSeconds;
        cfg->warningParams[1].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].speedRequirement;
        cfg->warningParams[1].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].photoInterval;
        cfg->warningParams[1].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].firstLevelAlarmSpeed;
        cfg->warningParams[1].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].secondLevelAlarmSpeed;
        cfg->warningParams[2].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].enabled;
        cfg->warningParams[2].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].alarmInterval;
        cfg->warningParams[2].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].photoNumber;
        cfg->warningParams[2].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].sensitivity;
        cfg->warningParams[2].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].recordVideoSeconds;
        cfg->warningParams[2].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].speedRequirement;
        cfg->warningParams[2].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].photoInterval;
        cfg->warningParams[2].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].firstLevelAlarmSpeed;
        cfg->warningParams[2].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].secondLevelAlarmSpeed;
        cfg->warningParams[3].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].enabled;
        cfg->warningParams[3].alarmInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].alarmInterval;
        cfg->warningParams[3].photoNumber = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].photoNumber;
        cfg->warningParams[3].sensitivity = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].sensitivity;
        cfg->warningParams[3].recordVideoSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].recordVideoSeconds;
        cfg->warningParams[3].speedRequirement = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].speedRequirement;
        cfg->warningParams[3].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].photoInterval;
        cfg->warningParams[3].firstLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].firstLevelAlarmSpeed;
        cfg->warningParams[3].secondLevelAlarmSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].secondLevelAlarmSpeed;
        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    int rk_adas_set_config(ADASConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        pthread_mutex_lock(&allConfig.config_lock);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].enabled = cfg->warningParams[0].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].alarmInterval = cfg->warningParams[0].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].photoNumber = cfg->warningParams[0].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].sensitivity = cfg->warningParams[0].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].recordVideoSeconds = cfg->warningParams[0].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].speedRequirement = cfg->warningParams[0].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].photoInterval = cfg->warningParams[0].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].firstLevelAlarmSpeed = cfg->warningParams[0].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[0].secondLevelAlarmSpeed = cfg->warningParams[0].secondLevelAlarmSpeed;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].enabled = cfg->warningParams[1].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].alarmInterval = cfg->warningParams[1].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].photoNumber = cfg->warningParams[1].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].sensitivity = cfg->warningParams[1].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].recordVideoSeconds = cfg->warningParams[1].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].speedRequirement = cfg->warningParams[1].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].photoInterval = cfg->warningParams[1].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].firstLevelAlarmSpeed = cfg->warningParams[1].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[1].secondLevelAlarmSpeed = cfg->warningParams[1].secondLevelAlarmSpeed;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].enabled = cfg->warningParams[2].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].alarmInterval = cfg->warningParams[2].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].photoNumber = cfg->warningParams[2].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].sensitivity = cfg->warningParams[2].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].recordVideoSeconds = cfg->warningParams[2].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].speedRequirement = cfg->warningParams[2].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].photoInterval = cfg->warningParams[2].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].firstLevelAlarmSpeed = cfg->warningParams[2].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[2].secondLevelAlarmSpeed = cfg->warningParams[2].secondLevelAlarmSpeed;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].enabled = cfg->warningParams[3].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].alarmInterval = cfg->warningParams[3].alarmInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].photoNumber = cfg->warningParams[3].photoNumber;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].sensitivity = cfg->warningParams[3].sensitivity;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].recordVideoSeconds = cfg->warningParams[3].recordVideoSeconds;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].speedRequirement = cfg->warningParams[3].speedRequirement;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].photoInterval = cfg->warningParams[3].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].firstLevelAlarmSpeed = cfg->warningParams[3].firstLevelAlarmSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.adasConfig.warningParam[3].secondLevelAlarmSpeed = cfg->warningParams[3].secondLevelAlarmSpeed;
        SaveLocalParam();
        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    int rk_get_debug_feature_config(DebugFeatureConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        memset(cfg, 0, sizeof(*cfg));
        cfg->runMode.hasLogFile = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.hasLogFile;
        cfg->runMode.systemVolume = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.systemVolume;
        cfg->runMode.driverBootPromptInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.driverBootPromptInterval;
        cfg->runMode.isTfCardCheckVoiceOn = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.isTfCardCheckVoiceOn;
        cfg->runMode.tfCardCheckVoiceInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.tfCardCheckVoiceInterval;
        cfg->debugMode.isVoiceInit = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.debugMode.isVoiceInit;
        cfg->debugMode.isDebugMode = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.debugMode.isDebugMode;
        cfg->debugMode.mockSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.debugMode.mockSpeed;

        cfg->network.wirelessHotspotEnabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.wirelessHotspotEnabled;
        strcpy(cfg->network.wifiConfig.ssid, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.wifiConfig.ssid);
        strcpy(cfg->network.wifiConfig.password, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.wifiConfig.password);
        strcpy(cfg->network.apnConfig.password, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.password);
        strcpy(cfg->network.apnConfig.name, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.name);
        strcpy(cfg->network.apnConfig.username, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.username);
        strcpy(cfg->network.apnConfig.mcc, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.mcc);
        strcpy(cfg->network.apnConfig.mnc, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.mnc);
        strcpy(cfg->network.apnConfig.server, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.server);
        strcpy(cfg->network.apnConfig.proxy, allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.proxy);
        cfg->network.apnConfig.authType = (APNAuthType_t)allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.authType;
        cfg->network.apnConfig.enabled = allConfig.AllConfigDef.automangeset.WirelessSet.WirelessEnable;

        cfg->speedWarning.channels[0].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].enabled;
        cfg->speedWarning.channels[0].channel = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].channel;
        cfg->speedWarning.channels[0].speedThreshold = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].speedThreshold;
        cfg->speedWarning.channels[0].preWarningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].preWarningInterval;
        cfg->speedWarning.channels[0].preWarningSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].preWarningSpeed;
        cfg->speedWarning.channels[0].persistenceDuration = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].persistenceDuration;
        cfg->speedWarning.channels[0].warningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].warningInterval;
        cfg->speedWarning.channels[0].photoCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].photoCount;
        cfg->speedWarning.channels[0].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].photoInterval;
        cfg->speedWarning.channels[0].videoRecordSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].videoRecordSeconds;

        cfg->speedWarning.channels[1].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].enabled;
        cfg->speedWarning.channels[1].channel = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].channel;
        cfg->speedWarning.channels[1].speedThreshold = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].speedThreshold;
        cfg->speedWarning.channels[1].preWarningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].preWarningInterval;
        cfg->speedWarning.channels[1].preWarningSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].preWarningSpeed;
        cfg->speedWarning.channels[1].persistenceDuration = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].persistenceDuration;
        cfg->speedWarning.channels[1].warningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].warningInterval;
        cfg->speedWarning.channels[1].photoCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].photoCount;
        cfg->speedWarning.channels[1].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].photoInterval;
        cfg->speedWarning.channels[1].videoRecordSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].videoRecordSeconds;

        cfg->speedWarning.channels[2].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].enabled;
        cfg->speedWarning.channels[2].channel = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].channel;
        cfg->speedWarning.channels[2].speedThreshold = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].speedThreshold;
        cfg->speedWarning.channels[2].preWarningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].preWarningInterval;
        cfg->speedWarning.channels[2].preWarningSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].preWarningSpeed;
        cfg->speedWarning.channels[2].persistenceDuration = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].persistenceDuration;
        cfg->speedWarning.channels[2].warningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].warningInterval;
        cfg->speedWarning.channels[2].photoCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].photoCount;
        cfg->speedWarning.channels[2].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].photoInterval;
        cfg->speedWarning.channels[2].videoRecordSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].videoRecordSeconds;

        cfg->speedWarning.channels[3].enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].enabled;
        cfg->speedWarning.channels[3].channel = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].channel;
        cfg->speedWarning.channels[3].speedThreshold = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].speedThreshold;
        cfg->speedWarning.channels[3].preWarningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].preWarningInterval;
        cfg->speedWarning.channels[3].preWarningSpeed = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].preWarningSpeed;
        cfg->speedWarning.channels[3].persistenceDuration = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].persistenceDuration;
        cfg->speedWarning.channels[3].warningInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].warningInterval;
        cfg->speedWarning.channels[3].photoCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].photoCount;
        cfg->speedWarning.channels[3].photoInterval = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].photoInterval;
        cfg->speedWarning.channels[3].videoRecordSeconds = allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].videoRecordSeconds;
        return 0;
    }

    int rk_set_debug_feature_config(DebugFeatureConfig_t *cfg)
    {
        if (!cfg)
            return -1;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.hasLogFile = cfg->runMode.hasLogFile;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.systemVolume = cfg->runMode.systemVolume;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.dmsConfig.alarmVolume = cfg->runMode.systemVolume;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.driverBootPromptInterval = cfg->runMode.driverBootPromptInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.isTfCardCheckVoiceOn = cfg->runMode.isTfCardCheckVoiceOn;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.runMode.tfCardCheckVoiceInterval = cfg->runMode.tfCardCheckVoiceInterval;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.debugMode.isVoiceInit = cfg->debugMode.isVoiceInit;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.debugMode.isDebugMode = cfg->debugMode.isDebugMode;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.debugMode.mockSpeed = cfg->debugMode.mockSpeed;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.wirelessHotspotEnabled = cfg->network.wirelessHotspotEnabled;
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.wifiConfig.ssid, cfg->network.wifiConfig.ssid);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.wifiConfig.password, cfg->network.wifiConfig.password);
        strcpy(allConfig.AllConfigDef.automangeset.WirelessSet.LogPWD, cfg->network.apnConfig.password);
        strcpy(allConfig.AllConfigDef.automangeset.WirelessSet.WirelessAccess, cfg->network.apnConfig.name);
        strcpy(allConfig.AllConfigDef.automangeset.WirelessSet.LogUser, cfg->network.apnConfig.username);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.mcc, cfg->network.apnConfig.mcc);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.mnc, cfg->network.apnConfig.mnc);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.server, cfg->network.apnConfig.server);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.proxy, cfg->network.apnConfig.proxy);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.network.apnConfig.authType = (APNAuthType)cfg->network.apnConfig.authType;
        allConfig.AllConfigDef.automangeset.WirelessSet.WirelessEnable = cfg->network.apnConfig.enabled;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].enabled = cfg->speedWarning.channels[0].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].channel = cfg->speedWarning.channels[0].channel;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].speedThreshold = cfg->speedWarning.channels[0].speedThreshold;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].preWarningInterval = cfg->speedWarning.channels[0].preWarningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].preWarningSpeed = cfg->speedWarning.channels[0].preWarningSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].persistenceDuration = cfg->speedWarning.channels[0].persistenceDuration;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].warningInterval = cfg->speedWarning.channels[0].warningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].photoCount = cfg->speedWarning.channels[0].photoCount;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].photoInterval = cfg->speedWarning.channels[0].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].videoRecordSeconds = cfg->speedWarning.channels[0].videoRecordSeconds;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].enabled = cfg->speedWarning.channels[1].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].channel = cfg->speedWarning.channels[1].channel;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].speedThreshold = cfg->speedWarning.channels[1].speedThreshold;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].preWarningInterval = cfg->speedWarning.channels[1].preWarningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].preWarningSpeed = cfg->speedWarning.channels[1].preWarningSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].persistenceDuration = cfg->speedWarning.channels[1].persistenceDuration;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].warningInterval = cfg->speedWarning.channels[1].warningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].photoCount = cfg->speedWarning.channels[1].photoCount;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].photoInterval = cfg->speedWarning.channels[1].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].videoRecordSeconds = cfg->speedWarning.channels[1].videoRecordSeconds;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].enabled = cfg->speedWarning.channels[2].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].channel = cfg->speedWarning.channels[2].channel;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].speedThreshold = cfg->speedWarning.channels[2].speedThreshold;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].preWarningInterval = cfg->speedWarning.channels[2].preWarningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].preWarningSpeed = cfg->speedWarning.channels[2].preWarningSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].persistenceDuration = cfg->speedWarning.channels[2].persistenceDuration;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].warningInterval = cfg->speedWarning.channels[2].warningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].photoCount = cfg->speedWarning.channels[2].photoCount;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].photoInterval = cfg->speedWarning.channels[2].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].videoRecordSeconds = cfg->speedWarning.channels[2].videoRecordSeconds;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].enabled = cfg->speedWarning.channels[3].enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].channel = cfg->speedWarning.channels[3].channel;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].speedThreshold = cfg->speedWarning.channels[3].speedThreshold;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].preWarningInterval = cfg->speedWarning.channels[3].preWarningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].preWarningSpeed = cfg->speedWarning.channels[3].preWarningSpeed;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].persistenceDuration = cfg->speedWarning.channels[3].persistenceDuration;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].warningInterval = cfg->speedWarning.channels[3].warningInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].photoCount = cfg->speedWarning.channels[3].photoCount;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].photoInterval = cfg->speedWarning.channels[3].photoInterval;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].videoRecordSeconds = cfg->speedWarning.channels[3].videoRecordSeconds;
        SaveLocalParam();
        return 0;
    }

    int rk_get_running_status(RunningStatus_t *RunStatus)
    {
        if (!RunStatus)
            return -1;

        memset(RunStatus, 0, sizeof(RunningStatus_t));

        SSYFI_GPS g_curGpsInfo;
        memset(&g_curGpsInfo, 0, sizeof(SSYFI_GPS));
        GetBackBoardGpsData(&g_curGpsInfo);

        char imei[32] = {0};
        server_log_init();

        pthread_mutex_lock(&allConfig.config_lock);

        RunStatus->basicStatus.temperature = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.basicStatus_t.temperature;
        RunStatus->basicStatus.cpuUsage = get_cpu_usage();
        RunStatus->basicStatus.memoryUsage = get_mem_usage();
        RunStatus->basicStatus.storageUsage = get_total_storage_usage();
        RunStatus->basicStatus.tfCardUsage = get_tfcard_usage();

        RunStatus->basicStatus.tfCardWriteNormal = (test_tfcard_write() > 0) ? 1 : 0;
        RunStatus->basicStatus.storageCapacityNormal = (check_all_partitions() == 0) ? 1 : 0;
        RunStatus->basicStatus.storageSpaceNormal = (check_all_storage_status() == 0) ? 1 : 0;
        RunStatus->basicStatus.tfCardFormatNormal = (check_all_tfcard_health()) ? 1 : 0;

        snprintf(RunStatus->basicStatus.date, sizeof(RunStatus->basicStatus.date), "%04d-%02d-%02d %02d:%02d:%02d",
                 allConfig.AllConfigDef.commonset.system_date.year,
                 allConfig.AllConfigDef.commonset.system_date.month,
                 allConfig.AllConfigDef.commonset.system_date.day,
                 allConfig.AllConfigDef.commonset.system_date.hour,
                 allConfig.AllConfigDef.commonset.system_date.minute,
                 allConfig.AllConfigDef.commonset.system_date.second);

        unsigned int seconds = get_system_uptime_seconds();
        snprintf(RunStatus->basicStatus.runTime, sizeof(RunStatus->basicStatus.runTime),
                 "%02u:%02u:%02u", seconds / 3600, (seconds % 3600) / 60, seconds % 60);

        RunStatus->basicStatus.temperature = get_thermal_zone1_temp();

        int status = MsNetdvr_Get4GStatus();
        RunStatus->mobileNetworkStatus.networkType = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.mobileNetworkStatus_t.networkType;
        RunStatus->mobileNetworkStatus.networkConnected = (status == 2) ? 1 : 0;

        RunStatus->mobileNetworkStatus.signalStrength = MsNetdvr_Get4GSignalValue();

        int ret = MsNetdvr_GetModuleIMEI(imei, sizeof(imei));
        if (ret == 1 && strlen(imei) > 0)
        {
            DVR_DEBUG("imei = %s\n", imei);
            strncpy(RunStatus->mobileNetworkStatus.imei, imei, sizeof(RunStatus->mobileNetworkStatus.imei) - 1);
            RunStatus->mobileNetworkStatus.imei[sizeof(RunStatus->mobileNetworkStatus.imei) - 1] = '\0';
        }
        else
        {
            RunStatus->mobileNetworkStatus.imei[0] = '\0';
        }

        // DVR通道状态
        RunStatus->dvrStatus.channel1Normal = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.dvrStatus_t.channel1Normal;
        RunStatus->dvrStatus.channel2Normal = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.dvrStatus_t.channel2Normal;
        RunStatus->dvrStatus.channel3Normal = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.dvrStatus_t.channel3Normal;
        RunStatus->dvrStatus.channel4Normal = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.dvrStatus_t.channel4Normal;

        // GPS状态
        RunStatus->gpsStatus.antennaConnected = allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.gpsStatus_t.antennaConnected;
        RunStatus->gpsStatus.positionStatus = g_curGpsInfo.cGpsStatus;
        RunStatus->gpsStatus.satelliteCount = (unsigned char)g_curGpsInfo.reserved[0];
        RunStatus->gpsStatus.speed = (float)g_curGpsInfo.kiloSpeed + ((float)g_curGpsInfo.mimiSpeed) / 100.0f;

        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    int rk_capture_get_records(CaptureRecordList_t *recordlist)
    {
        if (!recordlist)
            return -1;
        memset(recordlist, 0, sizeof(*recordlist));
        recordlist->count = allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.count;
        recordlist->records = (CaptureRecord_t *)malloc(recordlist->count * sizeof(CaptureRecord_t));
        if (!recordlist->records)
        {
            return -1;
        }
        strcpy(recordlist->records[0].driverId, allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[0].driverId);
        strcpy(recordlist->records[0].driverName, allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[0].driverName);
        strcpy(recordlist->records[0].photoPath, allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[0].photoPath);
        recordlist->records[0].captureTime = allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[0].captureTime; // 当前时间
        recordlist->records[0].verifyResult = allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[0].verifyResult;

        strcpy(recordlist->records[1].driverId, allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[1].driverId);
        strcpy(recordlist->records[1].driverName, allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[1].driverName);
        strcpy(recordlist->records[1].photoPath, allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[1].photoPath);
        recordlist->records[1].captureTime = allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[1].captureTime; // 当前时间
        recordlist->records[1].verifyResult = allConfig.AllConfigDef.abdparam.IdaCfgSet.capturerecordlist_s.records[1].verifyResult;
        // strcpy(recordlist->records->driverName,allConfig.AllConfigDef.automangeset.BasicInfo.DriverName);
        return 0;
    }

    int rk_format_tf_card()
    {
        return -1;
    }

    int rk_driver_delete_info()
    {
        return 0;
    }

    int ms_record_play_control(const PlayControl_t *control)
    {
        if (!control)
            return -1;
        ms_rtmp_palyback_play_control(control->operate, control->playSpeed, control->dragTime);
        return 0;
    }

    void formatTime(time_t t, char *buf, size_t bufsize)
    {
        struct tm *tm_time = localtime(&t);
        strftime(buf, bufsize, "%Y%m%d-%H%M%S", tm_time);
    }

    int rk_get_record_file_list(RecordQueryParams_t *params, char *recordList)
    {
        if (!params || !recordList)
            return -1;
        int recordNum = MsNetdvr_SearchRecord(
            params->channel,
            params->startTime,
            params->endTime,
            params->recordType,
            NULL,
            params->maxNumPerPage,
            params->currentPage,
            1,
            recordList,
            params->bufferLength);
        printf("Raw RecordList:\n");
        for (int i = 0; i < 100; ++i)
        {
            printf("%02X ", (unsigned char)recordList[i]);
        }
        printf("\n");

        printf("RecordList as string:\n%s\n", recordList);
        return recordNum;
    }

    int rk_record_file_play(PlayBack_t *params, MS_LOCAL_REC_INFO *info)
    {
        if (!params || !info)
        {
            printf("[ERROR] rk_record_file_play: params/info NULL\n");
            return -1;
        }

        printf("[DEBUG] rk_record_file_play: 输入参数 channel=%d filename_len=%d filename=\"%s\"\n",
               params->channel, params->filename_len, params->filename);

        int copy_len = params->filename_len;
        if (copy_len > 255)
            copy_len = 255;
        strncpy(info->fullpath, params->filename, copy_len);
        info->fullpath[copy_len] = '\0';

        FILE_INFO_T file_info;
        memset(&file_info, 0, sizeof(file_info));

        printf("[DEBUG] 调用 MsNetdvr_GetRecordFileInfo: fullpath=\"%s\"\n", info->fullpath);
        int ret = MsNetdvr_GetRecordFileInfo(info->fullpath, &file_info);
        printf("[DEBUG] MsNetdvr_GetRecordFileInfo 返回: %d\n", ret);

        if (ret != 0)
        {
            printf("[ERROR] MsNetdvr_GetRecordFileInfo failed: %s\n", info->fullpath);
            return -2;
        }

        // 打印 file_info 关键信息
        printf("[DEBUG] file_info: start_tim=%ld duration=%d chn_num=%d chn_mask=0x%llX file_sz=%u\n",
               file_info.start_tim, file_info.duration, file_info.chn_num, file_info.chn_mask, file_info.file_sz);
        printf("[DEBUG] file_info.fullpath: \"%s\"\n", file_info.fullpath);

        info->start_time = file_info.start_tim;
        info->duration = file_info.duration;
        info->chn_num = file_info.chn_num;
        info->chn_mask = file_info.chn_mask;
        info->first_pts = file_info.first_pts;
        info->last_pts = 0;
        strncpy(info->fullpath, file_info.fullpath, sizeof(info->fullpath) - 1);
        info->fullpath[sizeof(info->fullpath) - 1] = '\0';
        info->file_sz = file_info.file_sz;

        for (int i = 0; i < 4; ++i)
        {
            info->channel[i].channel = i;
            info->channel[i].video_codec = file_info.chn_arr[i].v_codec;
            info->channel[i].video_fps = file_info.chn_arr[i].v_fps;
            info->channel[i].video_width = file_info.chn_arr[i].v_width;
            info->channel[i].video_height = file_info.chn_arr[i].v_height;
            info->channel[i].audio_sample = file_info.chn_arr[i].a_sample;
            info->channel[i].audio_codec = file_info.chn_arr[i].a_codec;
            info->channel[i].audio_channels = file_info.chn_arr[i].a_channels;

            printf("[DEBUG] channel[%d]: v_codec=%d v_fps=%d v_width=%d v_height=%d a_sample=%d a_codec=%d a_channels=%d\n",
                   i, info->channel[i].video_codec, info->channel[i].video_fps, info->channel[i].video_width,
                   info->channel[i].video_height, info->channel[i].audio_sample,
                   info->channel[i].audio_codec, info->channel[i].audio_channels);
        }

        printf("[DEBUG] 返回 MS_LOCAL_REC_INFO: start_time=%ld duration=%d chn_num=%d chn_mask=0x%llX file_sz=%u fullpath=\"%s\"\n",
               info->start_time, info->duration, info->chn_num, info->chn_mask, info->file_sz, info->fullpath);

        return 0;
    }

    int rk_storage_get_stream_config(RecordStreamConfig_t *config)
    {
        if (!config)
            return -1;
        memset(config, 0, sizeof(*config));
        pthread_mutex_lock(&allConfig.config_lock);
        memset(config, 0, sizeof(*config));
        config->enableMainstream = allConfig.AllConfigDef.abdparam.IdaCfgSet.RecordStreamConfig.enableMainstream;
        config->enableSubstream = allConfig.AllConfigDef.abdparam.IdaCfgSet.RecordStreamConfig.enableSubstream;
        config->enableThirdstream = allConfig.AllConfigDef.abdparam.IdaCfgSet.RecordStreamConfig.enableThirdstream;
        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    int rk_storage_set_stream_config(const RecordStreamConfig_t *config)
    {
        if (!config)
            return -1;
        pthread_mutex_lock(&allConfig.config_lock);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.RecordStreamConfig.enableMainstream = config->enableMainstream;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.RecordStreamConfig.enableSubstream = config->enableSubstream;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.RecordStreamConfig.enableThirdstream = config->enableThirdstream;
        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    int rk_storage_get_config(StorageConfig_t *config)
    {
        if (!config)
            return -1;
        memset(config, 0, sizeof(*config));
        pthread_mutex_lock(&allConfig.config_lock);
        config->diskMode = allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.diskMode;
        config->fileNum = allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.fileNum;
        config->fileDuration = allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.fileDuration;
        config->diskSpace = allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.diskSpace;
        config->diskStatus = allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.diskStatus;
        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    int rk_storage_set_config(const StorageConfig_t *config)
    {
        if (!config)
            return -1;
        pthread_mutex_lock(&allConfig.config_lock);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.diskMode = config->diskMode;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.fileNum = config->fileNum;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.fileDuration = config->fileDuration;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.diskSpace = config->diskSpace;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.StorageConfig.diskStatus = config->diskStatus;
        pthread_mutex_unlock(&allConfig.config_lock);
        return 0;
    }

    // int rk_get_device_info(DeviceInfo_t *deviceif)
    // {
    //     if (!deviceif)
    //         return -1;
    //     strcpy(deviceif->model, "C807RK");
    //     deviceif->signalStrength = 100;
    //     return 0;
    // }

    int rk_storage_format(void)
    {
        int ret;
        DISK_FORMAT_MODE_CONFIG_S config;
        config.u8ManageDiskMode = 0;
        config.u32DiskFileNum = 500;
        config.u32DiskFileDuration = 0;
        DebugFeatureConfig_t cfg;
        rk_get_debug_feature_config(&cfg);
        if (cfg.runMode.isTfCardCheckVoiceOn)
        {
            PcmPlayer::Instance()->AddTTS("TF卡格式化完成");
        }

        ret = MsNetdvr_Disk_Format(config, DiskFormatStateCb);
        return ret;
    }

    void *TfCardCheckVoiceThread(void *arg)
    {
        DebugFeatureConfig_t cfg;
        rk_get_debug_feature_config(&cfg);
        if (cfg.runMode.isTfCardCheckVoiceOn)
        {
            while (1)
            {
                if (disk_run_state != last_voice_state)
                {
                    switch (disk_run_state)
                    {
                    case DISK_RUNING_STATE:
                        PcmPlayer::Instance()->AddTTS("TF卡运行正常");
                        break;
                    case DISK_NO_DISK_STATE:
                        PcmPlayer::Instance()->AddTTS("未检测到TF卡");
                        break;
                    case DISK_ERROR_STATE:
                        PcmPlayer::Instance()->AddTTS("TF卡异常重启设备");
                        break;
                    case DISK_STOP_STATE:
                        PcmPlayer::Instance()->AddTTS("TF卡未初始化");
                        break;
                    default:
                        PcmPlayer::Instance()->AddTTS("TF卡状态未知");
                        break;
                    }

                    last_voice_state = disk_run_state;
                }
                // sleep(interval);
                //  disk_run_state = DISK_STOP_STATE;
                //  PcmPlayer::Instance()->AddTTS("TF卡未初始化");
                sleep(cfg.runMode.tfCardCheckVoiceInterval);
            }
        }
        return NULL;
    }

    int rk_storage_get_disk_format_status()
    {
        return 1;
    }

    int rk_get_product_info(ProductInfo_t *proinfo)
    {
        if (!proinfo)
            return -1;
        char ccid[32] = {0};
        char version[64] = {0};
        char app_version[128] = {0};
        char imei[32] = {0};
        char modem_id[16];

        memset(proinfo, 0, sizeof(ProductInfo_t));

        rk_system_get_all_version(version, sizeof(version));
        version[sizeof(version) - 1] = '\0';

        rk_system_get_app_version(app_version, sizeof(app_version));
        app_version[sizeof(app_version) - 1] = '\0';

        strncpy(proinfo->basicInfo.model,
                allConfig.AllConfigDef.abdparam.IdaCfgSet.productinfo.basicInfo.model,
                sizeof(proinfo->basicInfo.model) - 1);
        proinfo->basicInfo.model[sizeof(proinfo->basicInfo.model) - 1] = '\0';

        strncpy(proinfo->basicInfo.appVersion, app_version,
                sizeof(proinfo->basicInfo.appVersion) - 1);
        proinfo->basicInfo.appVersion[sizeof(proinfo->basicInfo.appVersion) - 1] = '\0';

        strncpy(proinfo->basicInfo.firmwareVersion, version,
                sizeof(proinfo->basicInfo.firmwareVersion) - 1);
        proinfo->basicInfo.firmwareVersion[sizeof(proinfo->basicInfo.firmwareVersion) - 1] = '\0';

        int ret = MsNetdvr_GetSIMCCID(ccid, sizeof(ccid));
        if (ret == 1)
        {
            DVR_DEBUG("ccid = %s\n", ccid);
            strncpy(proinfo->networkInfo.iccid, ccid,
                    sizeof(proinfo->networkInfo.iccid) - 1);
            proinfo->networkInfo.iccid[sizeof(proinfo->networkInfo.iccid) - 1] = '\0';
        }

        int imei_ret = MsNetdvr_GetModuleIMEI(imei, sizeof(imei));
        if (imei_ret == 1 && strlen(imei) > 0)
        {
            DVR_DEBUG("imei = %s\n", imei);
            strncpy(proinfo->networkInfo.imei, imei,
                    sizeof(proinfo->networkInfo.imei) - 1);
            proinfo->networkInfo.imei[sizeof(proinfo->networkInfo.imei) - 1] = '\0';
        }

        // modem
        if (get_4g_modem_id(modem_id, sizeof(modem_id)) == 0)
        {
            strncpy(proinfo->networkInfo.modem,
                    modem_id,
                    sizeof(proinfo->networkInfo.modem) - 1);
            proinfo->networkInfo.modem[sizeof(proinfo->networkInfo.modem) - 1] = '\0';
        }
        return 0;
    }

    int server_listen_on_port(int port)
    {
        int server_fd;
        struct sockaddr_in addr;
        int opt = 1;

        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0)
        {
            perror("socket");
            return -1;
        }
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            perror("bind");
            close(server_fd);
            return -2;
        }
        if (listen(server_fd, 10) < 0)
        {
            perror("listen");
            close(server_fd);
            return -3;
        }
        printf("服务端已监听端口 %d\n", port);
        return server_fd;
    }

    int rk_set_device_config(DeviceConfig_t *deviceconfig)
    {
        if (!deviceconfig)
            return -1;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.enabled = deviceconfig->daylightSaving.enabled;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.useUTC = deviceconfig->daylightSaving.useUTC;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.year = deviceconfig->daylightSaving.startTime.year;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.month = deviceconfig->daylightSaving.startTime.month;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.weekOfMonth = deviceconfig->daylightSaving.startTime.weekOfMonth;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.dayOfWeek = deviceconfig->daylightSaving.startTime.dayOfWeek;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.hour = deviceconfig->daylightSaving.startTime.hour;

        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.year = deviceconfig->daylightSaving.endTime.year;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.month = deviceconfig->daylightSaving.endTime.month;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.weekOfMonth = deviceconfig->daylightSaving.endTime.weekOfMonth;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.dayOfWeek = deviceconfig->daylightSaving.endTime.dayOfWeek;
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.hour = deviceconfig->daylightSaving.endTime.hour;

        // strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.remotePlatform.ip , deviceconfig->remotePlatform.ip);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.remotePlatform.port = deviceconfig->remotePlatform.port;
        // allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.remotePlatform.isConnected = deviceconfig->remotePlatform.isConnected;
        int fd = server_listen_on_port(deviceconfig->remotePlatform.port);

        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.ip, deviceconfig->serverPlatform.ip);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.port = deviceconfig->serverPlatform.port;
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.id, deviceconfig->serverPlatform.id);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.isConnected = deviceconfig->serverPlatform.isConnected;
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.plateNumber, deviceconfig->serverPlatform.plateNumber);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.plateColor, deviceconfig->serverPlatform.plateColor);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.immediateReportEnabled = deviceconfig->serverPlatform.immediateReportEnabled;

        // 基本配置
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneCode, deviceconfig->basicConfig.timeZoneCode);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneName, deviceconfig->basicConfig.timeZoneName);
        strcpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneId, deviceconfig->basicConfig.timeZoneId);
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.language = deviceconfig->basicConfig.language; // 中文

        try_set_timezone_and_update(allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneCode, deviceconfig);

        // 实时视频配置
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[0] = deviceconfig->realTimeVideo.channelEnabled[0];
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[1] = deviceconfig->realTimeVideo.channelEnabled[1];
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[2] = deviceconfig->realTimeVideo.channelEnabled[2];
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[3] = deviceconfig->realTimeVideo.channelEnabled[3];

        // AHD配置
        for (int i = 0; i < 4; ++i)
        {
            allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.channelEnabled[i] = deviceconfig->ahdConfig.channelEnabled[i];

        }
        allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.activeChannel = deviceconfig->ahdConfig.activeChannel;
        DVR_DEBUG("allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.activeChannel = %d\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.activeChannel);
        SaveLocalParam();
        return 0;
    }

    int rk_get_device_config(DeviceConfig_t *deviceconfig)
    {
        if (!deviceconfig)
            return -1;
        DaylightSaving_t old_daylightSaving = g_deviceconfig.daylightSaving;
        char ip[32] = {0};
        memset(deviceconfig, 0, sizeof(*deviceconfig));
        deviceconfig->daylightSaving.enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.enabled;
        deviceconfig->daylightSaving.useUTC = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.useUTC;

        deviceconfig->daylightSaving.startTime.year = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.year;
        deviceconfig->daylightSaving.startTime.month = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.month;
        deviceconfig->daylightSaving.startTime.weekOfMonth = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.weekOfMonth;
        deviceconfig->daylightSaving.startTime.dayOfWeek = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.dayOfWeek;
        deviceconfig->daylightSaving.startTime.hour = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.startTime.hour;

        deviceconfig->daylightSaving.endTime.year = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.year;
        deviceconfig->daylightSaving.endTime.month = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.month;
        deviceconfig->daylightSaving.endTime.weekOfMonth = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.weekOfMonth;
        deviceconfig->daylightSaving.endTime.dayOfWeek = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.dayOfWeek;
        deviceconfig->daylightSaving.endTime.hour = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.daylightSaving.endTime.hour;
        DVR_DEBUG("[DST-CLIENT] enabled = %d, useUTC = %d",
                  deviceconfig->daylightSaving.enabled, deviceconfig->daylightSaving.useUTC);
        DVR_DEBUG("[DST-CLIENT] startTime: year = %d, month = %d, weekOfMonth = %d, dayOfWeek = %d, hour = %d",
                  deviceconfig->daylightSaving.startTime.year,
                  deviceconfig->daylightSaving.startTime.month,
                  deviceconfig->daylightSaving.startTime.weekOfMonth,
                  deviceconfig->daylightSaving.startTime.dayOfWeek,
                  deviceconfig->daylightSaving.startTime.hour);
        DVR_DEBUG("[DST-CLIENT] endTime: year = %d, month = %d, weekOfMonth = %d, dayOfWeek = %d, hour = %d",
                  deviceconfig->daylightSaving.endTime.year,
                  deviceconfig->daylightSaving.endTime.month,
                  deviceconfig->daylightSaving.endTime.weekOfMonth,
                  deviceconfig->daylightSaving.endTime.dayOfWeek,
                  deviceconfig->daylightSaving.endTime.hour);
        struct tm real_time;
        get_current_time_with_mode(deviceconfig, &real_time);
        memcpy(&g_deviceconfig, deviceconfig, sizeof(DeviceConfig_t));
        if (is_daylight_saving_changed(&old_daylightSaving, &deviceconfig->daylightSaving))
        {
            apply_dst_to_physical_time(&g_deviceconfig, 0);
        }

        // get_local_time_with_dst(&deviceconfig->daylightSaving, &real_time);

        // printf("当前本地时间（考虑夏令时）：%04d-%02d-%02d %02d:%02d:%02d\n",
        //        real_time.tm_year + 1900, real_time.tm_mon + 1, real_time.tm_mday,
        //        real_time.tm_hour, real_time.tm_min, real_time.tm_sec);

        if (get_interface_ip("wlan0", ip, sizeof(ip)) == 0)
        {
            deviceconfig->remotePlatform.isConnected = 1;
            strcpy(deviceconfig->remotePlatform.ip, ip);
        }
        else if (get_interface_ip("eth0", ip, sizeof(ip)) == 0)
        {
            deviceconfig->remotePlatform.isConnected = 1;
            strcpy(deviceconfig->remotePlatform.ip, ip);
        }
        else
        {
            printf("get wlan0 and eth0 ip failed!\n");
            strcpy(deviceconfig->remotePlatform.ip, "192.168.11.1");
            deviceconfig->remotePlatform.isConnected = 0;
        }

        // strcpy(deviceconfig->remotePlatform.ip , allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.remotePlatform.ip);
        deviceconfig->remotePlatform.port = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.remotePlatform.port;
        // deviceconfig->remotePlatform.isConnected = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.remotePlatform.isConnected;

        strcpy(deviceconfig->serverPlatform.ip, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.ip);
        deviceconfig->serverPlatform.port = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.port;
        strcpy(deviceconfig->serverPlatform.id, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.id);
        deviceconfig->serverPlatform.isConnected = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.isConnected;
        strcpy(deviceconfig->serverPlatform.plateNumber, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.plateNumber);
        strcpy(deviceconfig->serverPlatform.plateColor, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.plateColor);
        deviceconfig->serverPlatform.immediateReportEnabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.serverPlatform.immediateReportEnabled;

        // 基本配置
        strcpy(deviceconfig->basicConfig.timeZoneCode, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneCode);
        strcpy(deviceconfig->basicConfig.timeZoneName, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneName);
        strcpy(deviceconfig->basicConfig.timeZoneId, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.timeZoneId);
        deviceconfig->basicConfig.language = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.basicConfig.language; // 中文

        // 实时视频配置
        deviceconfig->realTimeVideo.channelEnabled[0] = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[0];
        deviceconfig->realTimeVideo.channelEnabled[1] = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[1];
        deviceconfig->realTimeVideo.channelEnabled[2] = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[2];
        deviceconfig->realTimeVideo.channelEnabled[3] = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[3];

        // AHD配置
        for (int i = 0; i < 4; ++i)
        {
            deviceconfig->ahdConfig.channelEnabled[i] = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.channelEnabled[i];
            // ApplyAhdAndStdConfig(deviceconfig);
        }
        deviceconfig->ahdConfig.activeChannel = allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.activeChannel;
        return 0;
    }

int SaveDriverInfoToFile(DriverInfo_t *driver)
{
    FILE *fp = fopen("/mnt/img/.hqtds", "w+");;
    if (fp == NULL) {
        printf("Failed to open file for saving driver info!\n");
        return -1;
    }

    int ret = fwrite(driver, sizeof(DriverInfo_t), 1, fp);
    if (ret != 1) {
        printf("Failed to write driver info to file!\n");
        fclose(fp);
        return -1;
    }

    fflush(fp);
    fclose(fp);
    return 0;
} 

int rk_add_verify_record(DriverInfo_t *driver, VerifyType_t verifyType, VerifyResult_t verifyResult)
{
    // if (driver == NULL) {
    //     return -1;  
    // }
    // VerifyRecordList_t verifyRecordList;
    // verifyRecordList.count++;
    // verifyRecordList.records = realloc(verifyRecordList.records, verifyRecordList.count * sizeof(VerifyRecord_t));
    // if (!verifyRecordList.records) {
    //     return -1; 
    // }

    // // 填充新验证记录
    // VerifyRecord_t* newRecord = &verifyRecordList.records[verifyRecordList.count - 1];
    // strncpy(newRecord->driverId, driver->driverId, sizeof(newRecord->driverId) - 1);
    // newRecord->driverId[sizeof(newRecord->driverId) - 1] = '\0';

    // strncpy(newRecord->driverName, driver->driverName, sizeof(newRecord->driverName) - 1);
    // newRecord->driverName[sizeof(newRecord->driverName) - 1] = '\0';

    // strncpy(newRecord->photoPath, driver->photoPath, sizeof(newRecord->photoPath) - 1);
    // newRecord->photoPath[sizeof(newRecord->photoPath) - 1] = '\0';

    // newRecord->verifyTime = time(NULL); 
    // newRecord->verifyType = verifyType;  
    // newRecord->verifyResult = verifyResult; 
    return 0; 
}

int rk_driver_get_list(DriverList_t *driverlist)
{
    if (!driverlist)
        return -1;

    memset(driverlist, 0, sizeof(*driverlist));

    driverlist->count = allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.count;
    driverlist->drivers = (DriverInfo_t *)malloc(driverlist->count * sizeof(DriverInfo_t));
    if (driverlist->drivers == NULL) {
        DVR_DEBUG("Memory allocation failed for driver list!");
        return -1;
    }

    DVR_DEBUG("driverlist->count = %d\n", driverlist->count);

    for (int i = 0; i < driverlist->count; ++i)
    {
        strncpy(driverlist->drivers[i].driverId, allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverId, sizeof(driverlist->drivers[i].driverId) - 1);
        driverlist->drivers[i].driverId[sizeof(driverlist->drivers[i].driverId) - 1] = '\0';
        
        strncpy(driverlist->drivers[i].driverName, allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverName, sizeof(driverlist->drivers[i].driverName) - 1);
        driverlist->drivers[i].driverName[sizeof(driverlist->drivers[i].driverName) - 1] = '\0';

        // strncpy(driverlist->drivers[i].photoPath, allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].photoPath, sizeof(driverlist->drivers[i].photoPath) - 1);
        // driverlist->drivers[i].photoPath[sizeof(driverlist->drivers[i].photoPath) - 1] = '\0';

        driverlist->drivers[i].hasPhoto = allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].hasPhoto;
        driverlist->drivers[i].registerTime = allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].registerTime;
        driverlist->drivers[i].isActive = allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].isActive;
        
        strncpy(driverlist->drivers[i].photoId, allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].photoId, sizeof(driverlist->drivers[i].photoId) - 1);
        driverlist->drivers[i].photoId[sizeof(driverlist->drivers[i].photoId) - 1] = '\0';

        DVR_DEBUG("driverlist->drivers[i].driverName = %s\n", driverlist->drivers[i].driverName);
    }

    return 0;
}


int rk_driver_register(DriverInfo_t *driver)
{
    if (driver == NULL) {
        return -1;
    }
    int currentDriverCount = allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.count;
    int driverExists = 0;

    for (int i = 0; i < currentDriverCount; ++i) {
        if (strcmp(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverId, driver->driverId) == 0) {
            driverExists = 1;
            strncpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverName, driver->driverName, sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverName) - 1);
            allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverName[sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].driverName) - 1] = '\0';

            allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].hasPhoto = driver->hasPhoto;
            allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].registerTime = driver->registerTime;
            allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].isActive = driver->isActive;

            strncpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].photoId, driver->photoId, sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].photoId) - 1);
            allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].photoId[sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[i].photoId) - 1] = '\0';
            // int ret = rk_add_verify_record(driver, VERIFY_TYPE_REGISTER, VERIFY_RESULT_SUCCESS);
            // if (ret != 0) {
            //     DVR_DEBUG("Failed to add verify record!");
            //     return -1;
            // }
            //SaveLocalParam();
            return 0;
        }
    }


    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.count = currentDriverCount + 1;

    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers = (DriverInfo *)realloc(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers, allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.count * sizeof(DriverInfo));
    if (allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers == NULL) {
        DVR_DEBUG("Memory reallocation failed for new driver!");
        return -1;
    }

    strncpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverId, driver->driverId, sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverId) - 1);
    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverId[sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverId) - 1] = '\0';

    strncpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverName, driver->driverName, sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverName) - 1);
    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverName[sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].driverName) - 1] = '\0';

    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].hasPhoto = driver->hasPhoto;
    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].registerTime = driver->registerTime;
    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].isActive = driver->isActive;

    strncpy(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].photoId, driver->photoId, sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].photoId) - 1);
    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].photoId[sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers[currentDriverCount].photoId) - 1] = '\0';

    //SaveLocalParam();
    // int ret = rk_add_verify_record(driver, VERIFY_TYPE_REGISTER, VERIFY_RESULT_SUCCESS);
    // if (ret != 0) {
    //     DVR_DEBUG("Failed to add verify record!");
    //     return -1;
    // }
    return 0;
}


int rk_verify_get_records(VerifyRecordList_t *verifyrecordlist)
{
        if (!verifyrecordlist)
            return -1;
        memset(verifyrecordlist, 0, sizeof(*verifyrecordlist));
        verifyrecordlist->count = allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.count;
        verifyrecordlist->records = (VerifyRecord_t *)malloc(verifyrecordlist->count * sizeof(VerifyRecord_t));
        DVR_DEBUG("driverlist->count = %d\n", verifyrecordlist->count);
       for (int i = 0; i < verifyrecordlist->count; ++i)
        {
            strcpy(verifyrecordlist->records[i].driverId, allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.records[i].driverId);
            strcpy(verifyrecordlist->records[i].driverName, allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.records[i].driverName);
            strcpy(verifyrecordlist->records[i].photoPath, allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.records[i].photoPath);
            verifyrecordlist->records[i].verifyTime = allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.records[i].verifyTime;
            verifyrecordlist->records[i].verifyType = (VerifyType_t)allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.records[i].verifyType;       // 类型转换，如果有typedef区别
            verifyrecordlist->records[i].verifyResult = (VerifyResult_t)allConfig.AllConfigDef.abdparam.IdaCfgSet.verirecordlist.records[i].verifyResult; // 类型转换，如果有typedef区别
            DVR_DEBUG("verifyrecordlist->records[i].driverId = %s\n", verifyrecordlist->records[i].driverId);
            DVR_DEBUG("verifyrecordlist->records[i].driverName = %s\n", verifyrecordlist->records[i].driverName);
            DVR_DEBUG("verifyrecordlist->records[i].photoPath = %s\n", verifyrecordlist->records[i].photoPath);
            DVR_DEBUG("verifyrecordlist->records[i].verifyTime = %s\n", ctime(&verifyrecordlist->records[i].verifyTime));
            switch (verifyrecordlist->records[i].verifyResult)
            {
            case VERIFY_TYPE_IDENTITY_t:
                printf("身份验证\n");
                break;
            case VERIFY_TYPE_REGISTER_t:
                printf("注册\n");
                break;
            default:
                printf("未知\n");
            }
        }
         return 0;
}

int rk_verify_clear_records()
{
    DVR_DEBUG("Enter rk_verify_clear_records\n");
    return 0;
}

int rk_capture_set_config(CaptureConfig_t *captureconfig)
{
    if (!captureconfig)
    return -1;
    allConfig.AllConfigDef.abdparam.IdaCfgSet.captuconfig.enabled = captureconfig->enabled;
    allConfig.AllConfigDef.abdparam.IdaCfgSet.captuconfig.interval = captureconfig->interval;
    return 0;
}

    int rk_capture_get_config(CaptureConfig_t *captureconfig)
    {
        if (!captureconfig)
            return -1;
        memset(captureconfig, 0, sizeof(*captureconfig));
        captureconfig->enabled = allConfig.AllConfigDef.abdparam.IdaCfgSet.captuconfig.enabled;
        captureconfig->interval = allConfig.AllConfigDef.abdparam.IdaCfgSet.captuconfig.interval;
        SaveLocalParam();
        return 0;
    }

    int rk_capture_clear_records()
    {
        return 0;
    }

int rk_clear_driver_register_info()
{
    DVR_DEBUG("Enter rk_clear_driver_register_info\n");
    memset(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers, 0, sizeof(allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers));
    allConfig.AllConfigDef.abdparam.IdaCfgSet.drivlist.count = 0;
    return 0;
}


#ifdef __cplusplus
}
#endif
/***********************************************************************************************************
**函数:DVR_GetConfig
**功能:获得相应通道和类型的参数
**输入参数:OPerTypeDef_t opt,DVR_U32_T channel
**返回值:DVR_U8_T *pResult
***********************************************************************************************************/
DVR_U32_T DVR_GetConfig(OPerTypeDef_t opt, DVR_U8_T *pResult, DVR_U32_T channel)
{
    AllConfigDef_t *pAllConfig = SystemGlobal_GetAllconfigDefContext();
    DVR_U32_T nRetSize = false;
    Dvr_Lock(&allConfig.init_mutexLock, &allConfig.config_lock);
    switch (opt)
    {
    case oper_recordctrol_type: // 录像控制
    {
        if (channel != DOORDVR_CHANNEL_NUM)
        {
            *pResult = pAllConfig->recordctrol.recordctrol[channel];
        }
        else
        {
            memcpy(pResult, &pAllConfig->recordctrol, sizeof(RecodCtrol_t));
        }
        nRetSize = true;
    }
    break;
    case oper_alarmctrol_type: // 报警控制
    {
        if (channel != DOORDVR_CHANNEL_NUM)
        {
            *pResult = pAllConfig->alarmctrol.alarm_ctrol[channel];
        }
        else
        {
            memcpy(pResult, pAllConfig->alarmctrol.alarm_ctrol, sizeof(DVR_U8_T) * DOORDVR_CHANNEL_NUM);
        }
        nRetSize = true;
    }
    break;
    case oper_alarmoutput1_type: // 全局 报警通道1 输出 控制
    {
        *pResult = (DVR_U8_T)(pAllConfig->alarmctrol.enable_output_channel1);
        nRetSize = true;
    }
    break;
    case oper_alarmoutput2_type: // 全局 报警通道2 输出 控制
    {
        *pResult = (DVR_U8_T)(pAllConfig->alarmctrol.enable_output_channel2);
        nRetSize = true;
    }
    break;
    case oper_advanceset_type: // 高级选项
        break;
    case oper_advanceset_userinfo: // 高级选项:用户信息
    {
        nRetSize = sizeof(UserInfo_t) * SYSTEM_MAX_USER_COUNT;
        memcpy(pResult, &pAllConfig->advanceset.usermanage, nRetSize);
    }
    break;
    case oper_advanceset_AbnormalDeal_type: // 高级选项:异常处理
    {
        nRetSize = sizeof(AbnormalDeal_t);
        memcpy(pResult, &pAllConfig->advanceset.abnormaldeal, nRetSize);
    }
    break;
    case oper_advanceset_systemmaintain_type: // 高级选项 :系统维护
    {
        nRetSize = sizeof(SystemMainTain_t);
        memcpy(pResult, &pAllConfig->advanceset.systemmaintain, nRetSize);
    }
    break;
    case oper_advanceset_outputadjust_type: // 高级选项 :输出调节
    {
        nRetSize = sizeof(Outputadjust_t);
        memcpy(pResult, &pAllConfig->advanceset.outputadjust, nRetSize);
    }
    break;

    case oper_brightness:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.vga_brightness;
    }
    break;

    case oper_contrast:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.vga_contrast;
    }
    break;

    case oper_saturation:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.vga_saturation;
    }
    break;

    case oper_volume:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.output_volume;
    }
    break;

    case oper_margin_left:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.left_margin;
    }
    break;

    case oper_margin_right:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.right_margin;
    }
    break;

    case oper_margin_top:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.top_margin;
    }
    break;

    case oper_margin_bottom:
    {
        *pResult = (DVR_U8_T)pAllConfig->advanceset.outputadjust.bottom_margin;
    }
    break;

    case oper_commonset_type: // 常规设置
    {
        nRetSize = sizeof(CommonSet_t);
        memcpy(pResult, &pAllConfig->commonset, nRetSize);
    }
    break;
    case oper_encoderset_type: // 编码设置
    {
        nRetSize = sizeof(EncoderSet_t);
        if (channel == DVR_ALL_CH_NUM)
        {
            nRetSize = sizeof(EncoderSet_t);
            memcpy(pResult, &pAllConfig->encoderset, nRetSize);
        }
        else
        {
            nRetSize = sizeof(EncoderParam_t);
            memcpy(pResult, &pAllConfig->encoderset.recorde_param[channel], nRetSize);
        }
    }
    break;
    case oper_recordset_type: // 录像设置:录像计划
    {

        if (channel == DOORDVR_CHANNEL_NUM)
        {
            nRetSize = sizeof(RecordPlan_t);
            memcpy(pResult, &pAllConfig->recordset.record_paln, nRetSize);
        }
        else
        {
            nRetSize = sizeof(RecordWeekPlan_t);
            memcpy(pResult, &pAllConfig->recordset.record_paln.record_week_plan[channel], nRetSize);
        }
    }
    break;
    case oper_recordset_RecPolicy_type: // 录像设置:录像策略
        *pResult = pAllConfig->recordset.RecPolicy;
        nRetSize = true;
        break;
    case oper_recordset_timelength_type: // 录像设置:录像时长
        *pResult = pAllConfig->recordset.record_time_length;
        nRetSize = true;
        break;
    case oper_ptzset_type: // 云台设置
    {
        nRetSize = sizeof(PTZParam_t);
        if (channel == DOORDVR_CHANNEL_NUM)
        {
            nRetSize *= DOORDVR_CHANNEL_NUM;
            memcpy(pResult, &pAllConfig->ptz_set.ptzparam, nRetSize);
        }
        else
        {
            memcpy(pResult, &pAllConfig->ptz_set.ptzparam[channel], nRetSize);
        }
    }
    break;
    case oper_networkparm_type: // 网络设置oper_abdparam_type
    {
        nRetSize = sizeof(NetworkParam_t);
        memcpy(pResult, &pAllConfig->networkparm, nRetSize);
    }
    break;
    case oper_alarmset_type: // 报警设置
    {
        nRetSize = sizeof(AlarmPlan_t);
        if (channel == DOORDVR_CHANNEL_NUM)
        {
            nRetSize *= DOORDVR_CHANNEL_NUM;
            memcpy(pResult, &pAllConfig->alarmset.alarmplan, nRetSize);
        }
        else
        {
            memcpy(pResult, &pAllConfig->alarmset.alarmplan[channel], nRetSize);
        }
    }
    break;
    case oper_alarmset_devtpye: // 报警输入常开 常闭
    {
        DVR_U8_T i;

        for (i = 0; i < DOORDVR_CHANNEL_NUM; i++)
        {
            pResult[i] = pAllConfig->alarmset.alarmplan[i].alarm_devtype;
        }
    }
    break;
    case oper_alarmset_delay: // 报警输出延时
        *pResult = pAllConfig->alarmset.alarmplan[0].alarm_delay;
        break;
    case oper_videodetectset_type: // 移动侦测
    {
        nRetSize = sizeof(Detect_t);
        if (channel == DOORDVR_CHANNEL_NUM)
        {
            nRetSize *= DOORDVR_CHANNEL_NUM;
            memcpy(pResult, &pAllConfig->videodetectset.motion_detect, nRetSize);
        }
        else
        {
            memcpy(pResult, &pAllConfig->videodetectset.motion_detect[channel], nRetSize);
        }
    }
    break;
    case oper_videolost_type: // 视频丢失
    {
        nRetSize = sizeof(Detect_t);
        if (channel == DOORDVR_CHANNEL_NUM)
        {
            nRetSize *= DOORDVR_CHANNEL_NUM;
            memcpy(pResult, &pAllConfig->videodetectset.videolost_detect, nRetSize);
        }
        else
        {
            memcpy(pResult, &pAllConfig->videodetectset.videolost_detect[channel], nRetSize);
        }
    }
    break;
    case oper_motion_type:
    {
        nRetSize = sizeof(Detect_t);
        if (channel == DOORDVR_CHANNEL_NUM)
        {
            nRetSize *= DOORDVR_CHANNEL_NUM;
            memcpy(pResult, &pAllConfig->videodetectset.motion_detect, nRetSize);
        }
        else
        {
            memcpy(pResult, &pAllConfig->videodetectset.motion_detect[channel], nRetSize);
        }
    }
    break;
    case oper_motionsensitivity:
    {
        memcpy(pResult, &pAllConfig->videodetectset.motion_detect[channel].montion_sensitivity, sizeof(DVR_U8_T));
    }
    break;
    case oper_motionarea:
    {
        memcpy(pResult, &pAllConfig->videodetectset.motion_aera[channel], MOTIOM_SIZE);
    }
    break;
    case oper_osdtimepos:
    {
        memcpy(pResult, &pAllConfig->encoderset.recorde_param[channel].timeoverly_pos, sizeof(OverlyPos_t));
    }
    break;
    case oper_osdcharpos:
    {
        memcpy(pResult, &pAllConfig->encoderset.recorde_param[channel].stringoverly_pos, sizeof(OverlyPos_t));
    }
    break;
    case oper_localdisplay:
    {
        if (DOORDVR_CHANNEL_NUM == channel)
        {
            memcpy(pResult, &pAllConfig->localdisplayset, sizeof(LocalDisplay_T));
        }
        else
        {
            memcpy(pResult, pAllConfig->localdisplayset.channel_name[channel], 32);
        }
    }
    break;
    case oper_default_layout_type:
    {
        memcpy(pResult, &pAllConfig->localdisplayset.default_layout, sizeof(DVR_U8_T));
    }
    break;
    case oper_cloudIpaddr:
    {
        memcpy(pResult, pAllConfig->networkparm.upnp.CloudServer, 64);
    }
    break;
    case oper_enableUpnPState:
    {
        memcpy(pResult, &pAllConfig->networkparm.upnp.enable_upnp, 2);
    }
    break;
    case oper_msserverconfig_type:
    {
        nRetSize = sizeof(MSServerConfig_t);
        memcpy(pResult, &pAllConfig->msserverconfig, nRetSize);
    }
    break;
    case oper_automange_type:
    {
        nRetSize = sizeof(AutoMangeParam_t);
        memcpy(pResult, &pAllConfig->automangeset, nRetSize);
    }
    break;
    case oper_cmsparam_type:
    {
        nRetSize = sizeof(CMSParam_t);
        memcpy(pResult, &pAllConfig->networkparm.cmsparam, nRetSize);
    }
    break;
    case oper_basicparam_type:
    {
        nRetSize = sizeof(BasicParam_t);
        memcpy(pResult, &pAllConfig->automangeset.BasicInfo, nRetSize);
    }
    break;
    case oper_wifiparam_type:
    {
        nRetSize = sizeof(WifiParam_t);
        memcpy(pResult, &pAllConfig->automangeset.WifiSet, nRetSize);
    }
    break;
    case oper_accparam_type:
    {
        nRetSize = sizeof(AccSetup_t);
        memcpy(pResult, &pAllConfig->automangeset.AccSet, nRetSize);
    }
    break;
    case oper_wirelessparam_type:
    {
        nRetSize = sizeof(WirelessParam_t);
        memcpy(pResult, &pAllConfig->automangeset.WirelessSet, nRetSize);
    }
    break;
    case oper_mdvrparam_type:
    {
        nRetSize = sizeof(MDvrParam_t);
        memcpy(pResult, &pAllConfig->automangeset.MDvrSet, nRetSize);
    }
    break;
    case oper_camerasetting_type:
    {
        nRetSize = sizeof(CameralSetting_t);
        memcpy(pResult, &pAllConfig->cameraset, nRetSize);
    }
    break;
    case oper_abdparam_type:
    {
        nRetSize = sizeof(ABDParam_t);
        memcpy(pResult, &pAllConfig->abdparam, nRetSize);
    }
    break;

    default:
        DVR_DEBUG("get param type is error !");
        break;
    }
    Dvr_UnLock(&allConfig.config_lock);
    return nRetSize;
}

/***********************************************************************************************************
**函数:DVR_SetConfig
**功能: 设置相应通道和类型的参数
**输入参数:
**返回值:
***********************************************************************************************************/
DVR_U32_T DVR_SetConfig(OPerTypeDef_t opt, DVR_U8_T *pResult, DVR_U32_T channel)
{
    //    size_t offallconfig;
    DVR_U32_T size;
    // DVR_U8_T buffer[32];
    AllConfigDef_t *pAllConfig = SystemGlobal_GetAllconfigDefContext();
    Dvr_Lock(&allConfig.init_mutexLock, &allConfig.config_lock);
    switch (opt)
    {
    case oper_recordctrol_type: // 录像控制
    {
        RecodCtrol_t *recordctrol_change;
        recordctrol_change = (RecodCtrol_t *)(void *)pResult;
        memcpy(&(pAllConfig->recordctrol), recordctrol_change, sizeof(RecodCtrol_t));
    }

    break;
    case oper_alarmctrol_type: // 报警控制
    {
        AlarmCtrol_t *alarmctrol_change;
        alarmctrol_change = (AlarmCtrol_t *)(void *)pResult;
        memcpy(&(pAllConfig->alarmctrol), alarmctrol_change, sizeof(AlarmCtrol_t));
    }
    break;
    case oper_advanceset_type: // 高级选项
    {
        AdvancedSet_t *advanceset_change;
        advanceset_change = (AdvancedSet_t *)(void *)pResult;
        memcpy(&(pAllConfig->advanceset), advanceset_change, sizeof(AdvancedSet_t));
    }
    break;

    case oper_advanceset_userinfo: // 用户信息
    {
        UserInfo_t *userinfo_change;
        userinfo_change = (UserInfo_t *)(void *)pResult;
        if (SYSTEM_MAX_USER_COUNT == channel)
        {
            size = sizeof(UserInfo_t) * SYSTEM_MAX_USER_COUNT;
            memcpy(&(pAllConfig->advanceset.usermanage), userinfo_change, size);
        }
        else
        {
            size = sizeof(UserInfo_t);
            memcpy(&(pAllConfig->advanceset.usermanage[channel]), userinfo_change, size);
            DVR_DEBUG("add user name %s %d,%d", pAllConfig->advanceset.usermanage[channel].user.username, pAllConfig->advanceset.usermanage[channel].user.valid,
                      pAllConfig->advanceset.usermanage[channel].user.user_id);
        }
    }
    break;

    case oper_brightness:
    {
        pAllConfig->advanceset.outputadjust.vga_brightness = (DVR_I8_T)(*pResult);
        // sprintf(buffer,"echo %d /proc/flcd200/brightness ",(DVR_I8_T )(*pResult));
        // DVR_DEBUG("%s",buffer);
    }
    break;

    case oper_contrast:
    {
        pAllConfig->advanceset.outputadjust.vga_contrast = *pResult;
        // sprintf(buffer, "echo %d /proc/flcd200/contrast ", (DVR_U8_T)(*pResult));
        // DVR_DEBUG("%s",buffer);
    }
    break;

    case oper_saturation:
    {
        pAllConfig->advanceset.outputadjust.vga_saturation = (DVR_I8_T)(*pResult);
        // sprintf(buffer,"echo %d /proc/flcd200/contrast ",(DVR_I8_T )(*pResult));
        // DVR_DEBUG("%s",buffer);
    }
    break;

    case oper_volume:
    {
        pAllConfig->advanceset.outputadjust.output_volume = *pResult;
    }
    break;

    case oper_margin_left:
    {
        pAllConfig->advanceset.outputadjust.left_margin = (DVR_I8_T)(*pResult);
    }
    break;

    case oper_margin_right:
    {
        pAllConfig->advanceset.outputadjust.right_margin = (DVR_I8_T)(*pResult);
    }
    break;

    case oper_margin_top:
    {
        pAllConfig->advanceset.outputadjust.top_margin = (DVR_I8_T)(*pResult);
    }
    break;

    case oper_margin_bottom:
    {
        pAllConfig->advanceset.outputadjust.bottom_margin = (DVR_I8_T)(*pResult);
    }
    break;

    case oper_commonset_type: // 常规设置
    {
        CommonSet_t *commonset_change;
        commonset_change = (CommonSet_t *)(void *)pResult;
        if (commonset_change->language != pAllConfig->commonset.language)
        {
            printf("PcmPlayer::Instance()->ReinitWavFileList();\n");
            // PcmPlayer::Instance()->ReinitWavFileList(commonset_change->language);
        }
        memcpy(&(pAllConfig->commonset), commonset_change, sizeof(CommonSet_t));
    }
    break;
    case oper_encoderset_type: // 编码设置
    {
        EncoderSet_t *encoderParam_change;
        encoderParam_change = (EncoderSet_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, encoderset);
        if (DVR_ALL_CH_NUM == channel)
        {
            memcpy(&pAllConfig->encoderset, encoderParam_change, sizeof(EncoderSet_t));
        }
        else
        {
            size = sizeof(EncoderParam_t);
            memcpy(&(pAllConfig->encoderset.recorde_param[channel]), encoderParam_change, size);
        }
    }
    break;

    case oper_recordset_type: // 录像设置
    {
        RecordSet_t *ecordSet_change;
        ecordSet_change = (RecordSet_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, recordset);
        size = sizeof(RecordSet_t);
        memcpy(&(pAllConfig->recordset), ecordSet_change, size);
    }
    break;
    case oper_recordset_RecPolicy_type:
    {
        DVR_U8_T *recPolicy_change;
        recPolicy_change = (DVR_U8_T *)(void *)pResult;

        //            offallconfig = offsetof(AllConfigDef_t, recordset);
        pAllConfig->recordset.RecPolicy = *recPolicy_change;
    }
    break;
    case oper_recordset_timelength_type:
    {
        DVR_U8_T *record_time_length_change;
        record_time_length_change = (DVR_U8_T *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, recordset);
        pAllConfig->recordset.record_time_length = *record_time_length_change;
    }
    break;
    case oper_ptzset_type: // 云台设置
    {
        PTZParam_t *ptz_set_change;
        ptz_set_change = (PTZParam_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, ptz_set);
        if (DOORDVR_CHANNEL_NUM == channel)
        {
            size = sizeof(PTZParam_t) * DOORDVR_CHANNEL_NUM;
            memcpy(pAllConfig->ptz_set.ptzparam, ptz_set_change, size);
        }
        else
        {
            size = sizeof(PTZParam_t);
            memcpy(&(pAllConfig->ptz_set.ptzparam[channel]), ptz_set_change, size);
        }
    }
    break;
    case oper_networkparm_type: // 网络设置
    {
        NetworkParam_t *networkparam_change;
        networkparam_change = (NetworkParam_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, networkparm);
        size = sizeof(NetworkParam_t);
        memcpy(&(pAllConfig->networkparm), networkparam_change, size);
    }
    break;
    case oper_alarmset_type: // 报警设置
    {
        AlarmPlan_t *alarmset_change;
        alarmset_change = (AlarmPlan_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, alarmset);
        if (DOORDVR_CHANNEL_NUM == channel)
        {
            size = sizeof(AlarmPlan_t) * DOORDVR_CHANNEL_NUM;
            memcpy(pAllConfig->alarmset.alarmplan, alarmset_change, size);
        }
        else
        {
            size = sizeof(AlarmPlan_t);
            memcpy(&(pAllConfig->alarmset.alarmplan[channel]), alarmset_change, size);
        }
    }
    break;
    case oper_videodetectset_type: // 视频检测
    {
        Detect_t *motion_detect_change;
        motion_detect_change = (Detect_t *)(void *)pResult;
        //           offallconfig = offsetof(AllConfigDef_t, videodetectset);
        if (DOORDVR_CHANNEL_NUM == channel)
        {
            size = sizeof(Detect_t) * DOORDVR_CHANNEL_NUM;
            memcpy(pAllConfig->videodetectset.motion_detect, motion_detect_change, size);
        }
        else
        {
            size = sizeof(Detect_t);
            memcpy(&(pAllConfig->videodetectset.motion_detect[channel]), motion_detect_change, size);
        }
    }
    break;
    case oper_videolost_type: // 视频丢失
    {
        Detect_t *videolost_detect_change;
        videolost_detect_change = (Detect_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, videodetectset) + sizeof(Detect_t) * 8;
        if (DOORDVR_CHANNEL_NUM == channel)
        {
            size = sizeof(Detect_t) * DOORDVR_CHANNEL_NUM;
            memcpy(pAllConfig->videodetectset.videolost_detect, videolost_detect_change, size);
        }
        else
        {
            size = sizeof(Detect_t);
            memcpy(&(pAllConfig->videodetectset.videolost_detect[channel]), videolost_detect_change, size);
        }
    }
    break;
    case oper_videolostmotion_type_part1:
    {
        VideoDetectSet_t *VideoDetectSet_change;
        VideoDetectSet_change = (VideoDetectSet_t *)(void *)pResult;
        size = sizeof(VideoDetectSet_t);
        memcpy(&(pAllConfig->videodetectset), VideoDetectSet_change, size);
    }
    break;
    case oper_videolostmotion_type_part2:
    {
        DVR_U8_T *VideoDetectSet_change;
        VideoDetectSet_change = (DVR_U8_T *)(void *)pResult;
        size = sizeof(VideoDetectSet_t) - sizeof(Detect_t) * 8;
        memcpy(&(pAllConfig->videodetectset.videolost_detect), VideoDetectSet_change, size);
    }

    break;
    case oper_localdisplay:
    {
        if (DOORDVR_CHANNEL_NUM == channel)
        {
            memcpy(&pAllConfig->localdisplayset, pResult, sizeof(LocalDisplay_T));
        }
        else
        {
            memcpy(&pAllConfig->localdisplayset.channel_name[channel], pResult, 32);
        }
    }
    break;
    case oper_advanceset_AbnormalDeal_type:
    {
        size = sizeof(AbnormalDeal_t);
        memcpy(&pAllConfig->advanceset.abnormaldeal, pResult, size);
    }
    break;
    case oper_msserverconfig_type:
    {
        MSServerConfig_t *msserverconfig_change;
        msserverconfig_change = (MSServerConfig_t *)(void *)pResult;
        //            offallconfig = offsetof(AllConfigDef_t, msserverconfig);
        size = sizeof(MSServerConfig_t);
        memcpy(&(pAllConfig->msserverconfig), msserverconfig_change, size);
    }
    break;
    case oper_automange_type:
    {
        AutoMangeParam_t *automange_change;
        automange_change = (AutoMangeParam_t *)(void *)pResult;
        size = sizeof(AutoMangeParam_t);
        memcpy(&(pAllConfig->automangeset), automange_change, size);
    }
    break;
    case oper_cmsparam_type:
    {
        CMSParam_t *cms_change;
        cms_change = (CMSParam_t *)(void *)pResult;
        size = sizeof(CMSParam_t);
        memcpy(&(pAllConfig->networkparm.cmsparam), cms_change, size);
    }
    break;
    case oper_basicparam_type:
    {
        BasicParam_t *basic_change;
        basic_change = (BasicParam_t *)(void *)pResult;
        size = sizeof(BasicParam_t);
        memcpy(&(pAllConfig->automangeset.BasicInfo), basic_change, size);
    }
    break;
    case oper_accparam_type:
    {
        AccSetup_t *acc_change;
        acc_change = (AccSetup_t *)(void *)pResult;
        size = sizeof(AccSetup_t);
        memcpy(&(pAllConfig->automangeset.AccSet), acc_change, size);
    }
    break;
    case oper_wirelessparam_type:
    {
        WirelessParam_t *wireless_chane;
        wireless_chane = (WirelessParam_t *)(void *)pResult;
        size = sizeof(WirelessParam_t);
        memcpy(&(pAllConfig->automangeset.WirelessSet), wireless_chane, size);
    }
    break;
    case oper_wifiparam_type:
    {
        WifiParam_t *wifi_chane;
        wifi_chane = (WifiParam_t *)(void *)pResult;
        size = sizeof(WifiParam_t);
        memcpy(&(pAllConfig->automangeset.WifiSet), wifi_chane, size);
    }
    break;
    case oper_mdvrparam_type:
    {
        MDvrParam_t *mdvr_chane;
        mdvr_chane = (MDvrParam_t *)(void *)pResult;
        size = sizeof(MDvrParam_t);
        memcpy(&(pAllConfig->automangeset.MDvrSet), mdvr_chane, size);
    }
    break;
    case oper_camerasetting_type:
    {
        CameralSetting_t *camera_chane;
        camera_chane = (CameralSetting_t *)(void *)pResult;
        size = sizeof(CameralSetting_t);
        memcpy(&(pAllConfig->cameraset), camera_chane, size);
    }
    break;
    case oper_abdparam_type:
    {
        StopUIHeartTime();

        int ret = -1;
        ABDParam_t *abd_param;
        abd_param = (ABDParam_t *)(void *)pResult;
        size = sizeof(ABDParam_t);
        DVR_DEBUG("abd_param->IdaCfgSet.adasConfig.alarmType=%3x,calibration=%d,PRODUCT_OR_TEST=%d\n",
                  abd_param->IdaCfgSet.adasConfig.alarmType, abd_param->IdaCfgSet.adasConfig.calibration, abd_param->AlgorithmSelect.PRODUCT_OR_TEST);
        DVR_DEBUG("send down channel number[DMS,ADAS,BSD1,BSD2,BSD3,BSD4]->[%d,%d,%d,%d,%d,%d]", abd_param->algVideoChnSelect.DMS_VideoChn_num,
                  abd_param->algVideoChnSelect.ADAS_VideoChn_num, abd_param->algVideoChnSelect.BSD1_VideoChn_num,
                  abd_param->algVideoChnSelect.BSD2_VideoChn_num, abd_param->algVideoChnSelect.BSD3_VideoChn_num,
                  abd_param->algVideoChnSelect.BSD4_VideoChn_num);

        if ((pAllConfig->abdparam.AlgorithmSelect.DMS_Enable != abd_param->AlgorithmSelect.DMS_Enable) || (pAllConfig->abdparam.AlgorithmSelect.ADAS_Enable != abd_param->AlgorithmSelect.ADAS_Enable) || (pAllConfig->abdparam.AlgorithmSelect.BSD_Enable != abd_param->AlgorithmSelect.BSD_Enable) || (pAllConfig->abdparam.AlgorithmSelect.PRODUCT_OR_TEST != abd_param->AlgorithmSelect.PRODUCT_OR_TEST) || ((pAllConfig->abdparam.AlgorithmSelect.BSD_Enable == abd_param->AlgorithmSelect.BSD_Enable == 1) && (pAllConfig->abdparam.IdaCfgSet.bsdConfig.BSDCount != abd_param->IdaCfgSet.bsdConfig.BSDCount)))
        {
            DVR_DEBUG("abd_param->AlgorithmSelect.DMS_Enable=%d,ADAS_Enable=%d,BSD_Enable=%d,PRODUCT_OR_TEST=%d,BSDCount=%d\n",
                      abd_param->AlgorithmSelect.DMS_Enable, abd_param->AlgorithmSelect.ADAS_Enable, abd_param->AlgorithmSelect.BSD_Enable,
                      abd_param->AlgorithmSelect.PRODUCT_OR_TEST, abd_param->IdaCfgSet.bsdConfig.BSDCount);
            DVR_DEBUG("pAllConfig->abdparam.AlgorithmSelect.DMS_Enable=%d,ADAS_Enable=%d,BSD_Enable=%d,PRODUCT_OR_TEST=%d,BSDCount=%d\n",
                      pAllConfig->abdparam.AlgorithmSelect.DMS_Enable, pAllConfig->abdparam.AlgorithmSelect.ADAS_Enable, pAllConfig->abdparam.AlgorithmSelect.BSD_Enable,
                      pAllConfig->abdparam.AlgorithmSelect.PRODUCT_OR_TEST, pAllConfig->abdparam.IdaCfgSet.bsdConfig.BSDCount);
#if 0
            pAllConfig->abdparam.AlgorithmSelect.DMS_Enable = abd_param->AlgorithmSelect.DMS_Enable;
            pAllConfig->abdparam.AlgorithmSelect.BSD_Enable = abd_param->AlgorithmSelect.BSD_Enable;
            pAllConfig->abdparam.AlgorithmSelect.ADAS_Enable = abd_param->AlgorithmSelect.ADAS_Enable;
            pAllConfig->abdparam.AlgorithmSelect.PRODUCT_OR_TEST = abd_param->AlgorithmSelect.PRODUCT_OR_TEST;         
            pAllConfig->abdparam.IdaCfgSet.bsdConfig.BSDCount = abd_param->IdaCfgSet.bsdConfig.BSDCount;
#endif
            ret = auth_test_main(abd_param);
            DVR_DEBUG("auth_test_main ret=%d\n", ret);
            if (ret == 0)
            {
                DVR_DEBUG("auth_test_main success,ret =%d\n", ret);
                memcpy(&(pAllConfig->abdparam), abd_param, size);
                CIDAConfig::Instance()->GetIdaParamAll(&(pAllConfig->abdparam));
            }
            else
            {
                DVR_DEBUG("auth_test_main failed,ret =%d\n", ret);
                // MessageQueue_Send_Process(MSP_CMD_LICENSE_GET_FAIL, NULL, 0, SEND_MESSAGE_TO_UI);
            }
            // StartUIHeartTime();
            break;
        }

        DVR_DEBUG("pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark =%d,abd_param->adasAditonCtrl.open_close_laneline_mark=%d",
                  pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark, abd_param->adasAditonCtrl.open_close_laneline_mark);

        if (pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark != abd_param->adasAditonCtrl.open_close_laneline_mark)
        {
            if (pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark == 0)
            {
                DVR_DEBUG("CIDAFrameDraw::Instance()->Start() before");
                CIDAFrameDraw::Instance()->Start();
                DVR_DEBUG("CIDAFrameDraw::Instance()->Start() after");
            }
            else
            {
                DVR_DEBUG("CIDAFrameDraw::Instance()->Stop() before");
                CIDAFrameDraw::Instance()->Stop();
                CIDAFrameDraw::Instance()->ClearFrameDraw();
                DVR_DEBUG("CIDAFrameDraw::Instance()->Stop() after");
            }
            pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark = abd_param->adasAditonCtrl.open_close_laneline_mark;
        }
        DVR_DEBUG("pAllConfig->abdparam.adasAditonCtrl.open_close_yuv_capture =%d,abd_param->adasAditonCtrl.open_close_yuv_capture=%d",
                  pAllConfig->abdparam.adasAditonCtrl.open_close_yuv_capture, abd_param->adasAditonCtrl.open_close_yuv_capture);

        if (pAllConfig->abdparam.adasAditonCtrl.open_close_yuv_capture != abd_param->adasAditonCtrl.open_close_yuv_capture)
        {
            if (pAllConfig->abdparam.adasAditonCtrl.open_close_yuv_capture == 0)
            {
                DVR_DEBUG("begin  CheckUsbDiskDev ");
                CheckUsbDiskDev(NULL);
                DVR_DEBUG("end  CheckUsbDiskDev ");
            }
            pAllConfig->abdparam.adasAditonCtrl.open_close_yuv_capture = abd_param->adasAditonCtrl.open_close_yuv_capture;
        }

        DVR_DEBUG("pAllConfig->abdparam.algVideoChnSelect.ADAS_VideoChn_num=%d,abd_param->algVideoChnSelect.ADAS_VideoChn_num=%d",
                  pAllConfig->abdparam.algVideoChnSelect.ADAS_VideoChn_num, abd_param->algVideoChnSelect.ADAS_VideoChn_num);
        if ((pAllConfig->abdparam.algVideoChnSelect.ADAS_VideoChn_num == abd_param->algVideoChnSelect.ADAS_VideoChn_num))
        {

            if (memcmp(&(pAllConfig->abdparam.IdaCfgSet.adasConfig), &(abd_param->IdaCfgSet.adasConfig), sizeof(ADAS_CONFIG_S)) != 0)
            {
                DVR_DEBUG("ADAS_VideoChn_num no changed, but adasConfig has changed");

                DVR_DEBUG("abd_param->IdaCfgSet.adasConfig.AlarmParam[i].speedThreshold[%d,%d,%d,%d]", abd_param->IdaCfgSet.adasConfig.AlarmParam[0].speedThreshold,
                          abd_param->IdaCfgSet.adasConfig.AlarmParam[1].speedThreshold, abd_param->IdaCfgSet.adasConfig.AlarmParam[2].speedThreshold, abd_param->IdaCfgSet.adasConfig.AlarmParam[3].speedThreshold);
                DVR_DEBUG("abd_param->IdaCfgSet.adasConfig.AlarmParam[i].interval[%d,%d,%d,%d]", abd_param->IdaCfgSet.adasConfig.AlarmParam[0].interval,
                          abd_param->IdaCfgSet.adasConfig.AlarmParam[1].interval, abd_param->IdaCfgSet.adasConfig.AlarmParam[2].interval, abd_param->IdaCfgSet.adasConfig.AlarmParam[3].interval);

                DVR_DEBUG("pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[i].speedThreshold[%d,%d,%d,%d]", pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[0].speedThreshold,
                          pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[1].speedThreshold, pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[2].speedThreshold, pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[3].speedThreshold);
                DVR_DEBUG("pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[i].interval[%d,%d,%d,%d]", pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[0].interval,
                          pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[1].interval, pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[2].interval, pAllConfig->abdparam.IdaCfgSet.adasConfig.AlarmParam[3].interval);

                if (memcmp(&(pAllConfig->abdparam.IdaCfgSet.adasConfig.CalibInfo), &(abd_param->IdaCfgSet.adasConfig.CalibInfo), sizeof(ADAS_CALIB_INFO_S)) != 0)
                {
                    DVR_DEBUG("setNeedCaliFlag(true)");
                    IntelligentDriverAssistant::Instance()->setNeedCaliFlag(true);
                }
                else
                {
                    DVR_DEBUG("setNeedCaliFlag(false)");
                    IntelligentDriverAssistant::Instance()->setNeedCaliFlag(false);
                }

                if (pAllConfig->abdparam.algVideoChnSelect.ADAS_VideoChn_num != 0)
                {
                    CADASDetect::Instance()->Pause();
                }

                memcpy(&(pAllConfig->abdparam.IdaCfgSet.adasConfig), &(abd_param->IdaCfgSet.adasConfig), sizeof(ADAS_CONFIG_S));

                if (abd_param->algVideoChnSelect.ADAS_VideoChn_num != 0)
                {
                    DVR_U8_T encodeSize;
                    int adasChannelNum = pAllConfig->abdparam.algVideoChnSelect.ADAS_VideoChn_num - 1;
                    if (adasChannelNum >= 0)
                        encodeSize = pAllConfig->encoderset.recorde_param[adasChannelNum].encoder_size;
                    else
                        encodeSize = pAllConfig->encoderset.recorde_param[5].encoder_size;

                    DVR_DEBUG("pAllConfig->encoderset.recorde_param[%d]->encoder_size=%d\n", adasChannelNum, encodeSize);

                    CIDAConfig::Instance()->GetIdaParamAll(&(pAllConfig->abdparam));
                    // IntelligentDriverAssistant::Instance()->initADASHandle(abd_param->algVideoChnSelect.ADAS_VideoChn_num, encodeSize);
                    DVR_DEBUG("abd_param->algVideoChnSelect.ADAS_VideoChn_num=%d,encodeSize=%d", abd_param->algVideoChnSelect.ADAS_VideoChn_num, encodeSize);

                    IntelligentDriverAssistant::Instance()->ms_SetAdasParam();
                    DVR_DEBUG("ms_SetAdasParam");

                    if (IntelligentDriverAssistant::Instance()->getNeedCaliFlag() == true)
                    {
                        DVR_DEBUG("ms_SetAdasCalibrate");
                        IntelligentDriverAssistant::Instance()->ms_SetAdasCalibrate();
                    }

#if 0
                    IntelligentDriverAssistant::Instance()->initADASHandle(1, 6);	  
                    CADASDetect::Instance()->Start();
#else
                    CADASDetect::Instance()->Resume();
#endif
                }

                // StartUIHeartTime();
                break;
            }
            else
            {
                CIDAConfig::Instance()->GetIdaParamAll(&(pAllConfig->abdparam));
                DVR_DEBUG("pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark=%d,open_close_yuv_capture=%d",
                          pAllConfig->abdparam.adasAditonCtrl.open_close_laneline_mark, pAllConfig->abdparam.adasAditonCtrl.open_close_yuv_capture);
            }
        }

        // DVR_DEBUG("  before StartUIHeartTime()");
        // StartUIHeartTime();
        // DVR_DEBUG(" after  StartUIHeartTime()");
    }
        DVR_DEBUG("   ");

        break;

    default:
        DVR_DEBUG("get param type is error !");
        break;
    }
    Dvr_UnLock(&allConfig.config_lock);
    return 0;
}

void LoadDefaultRecordCtrol(RecodCtrol_t *recordctrl)
{
    if (recordctrl == NULL)
    {
        DVR_DEBUG("param is invalid !");
        return;
    }
    for (int i = 0; i < sizeof(recordctrl->recordctrol); i++)
    {
        recordctrl->recordctrol[i] = 2;
    }
}

void LoadDefaultAlarmCtrol(AlarmCtrol_t *alarmctrl)
{
    if (alarmctrl == NULL)
    {
        DVR_DEBUG("param is invalid !");
        return;
    }

    memset(alarmctrl, 0, sizeof(AlarmCtrol_t));
    alarmctrl->enable_output_channel1 = 1; // default value
}

void LoadDefaultAdvancedSet(AdvancedSet_t *advancedset)
{
    if (advancedset == NULL)
    {
        DVR_DEBUG("param is invalid !");
        return;
    }

    // 用户恢复默认
    memset(advancedset->usermanage, 0, sizeof(UserInfo_t));
    strcpy(advancedset->usermanage[0].user.username, "admin");
    memset(advancedset->usermanage[0].user.password, 0, sizeof(advancedset->usermanage[0].user.password));
    advancedset->usermanage[0].user.valid = 1;
    memset(&advancedset->usermanage[0].right, 0xff, sizeof(advancedset->usermanage[0].right));
    sprintf(advancedset->usermanage[0].user.password, "456789");

    strcpy(advancedset->usermanage[1].user.username, "default");
    memset(advancedset->usermanage[1].user.password, 0, sizeof(advancedset->usermanage[1].user.password));
    advancedset->usermanage[1].user.valid = 1;
    memset(&advancedset->usermanage[1].right, 0xff, sizeof(advancedset->usermanage[1].right));

    sprintf(advancedset->usermanage[1].user.password, "456789");
    advancedset->usermanage[1].right.recover_parma = 0;
    advancedset->usermanage[1].right.system_update = 0;
    advancedset->usermanage[1].right.alarm_set = 0;

    // 异常处理没有功能，直接清空
    memset((void *)&advancedset->abnormaldeal, 0, sizeof(AbnormalDeal_t));

    // 系统维护
    advancedset->systemmaintain.enable_reboot = 1;
    advancedset->systemmaintain.reboot_time.ulTime = 0;
    advancedset->systemmaintain.reboot_type = 0;
    advancedset->systemmaintain.systemupdate_source = 0;

    // 输出调整
    advancedset->outputadjust.vga_brightness = 50;
    advancedset->outputadjust.vga_contrast = 50;
    advancedset->outputadjust.vga_saturation = 64;
    advancedset->outputadjust.vga_resolution_ratio = 1;
    advancedset->outputadjust.output_volume = 12;
    advancedset->outputadjust.bottom_margin = 0;
    advancedset->outputadjust.top_margin = 0;
    advancedset->outputadjust.left_margin = 0;
    advancedset->outputadjust.right_margin = 0;
}

void LoadDefaultCommonSet(CommonSet_t *commonset)
{
    memset(commonset, 0, sizeof(CommonSet_t));

    sprintf(commonset->machine_num, "10");
    commonset->date_format = 0;
    commonset->time_format = 0;
    commonset->language = 1;
    commonset->video_format = SYSTEM_NTSC;
    commonset->gps_enable = 1;
    commonset->timezone = 8;
    commonset->reserve = 0;
}

void LoadDefaultEncoderSet(EncoderSet_t *encoderset)
{
    memset(encoderset, 0, sizeof(EncoderSet_t));

    for (int i = 0; i < 4; i++)
    {
        encoderset->recorde_param[i].timeoverly_pos.pos_x = 66;
        encoderset->recorde_param[i].timeoverly_pos.pos_y = 10;

        encoderset->recorde_param[i].stringoverly_pos.pos_x = 10;
        encoderset->recorde_param[i].stringoverly_pos.pos_y = 10;

        encoderset->recorde_param[i].gpsoverly_pos.pos_x = 10; //
        encoderset->recorde_param[i].gpsoverly_pos.pos_y = 88; //

        encoderset->recorde_param[i].plateoverly_pos.pos_x = 10; //
        encoderset->recorde_param[i].plateoverly_pos.pos_y = 80; //

        encoderset->recorde_param[i].encoder_size = ENCODER_720P; // ENCODER_D1;
        encoderset->recorde_param[i].bitstreame_control = DYNAMIC_BITSTREAM;
        encoderset->recorde_param[i].bitstreame_quality = StreamQuality_BEST;
        encoderset->recorde_param[i].bitstreame_size = 3;
        encoderset->recorde_param[i].framerate = 30;
        encoderset->recorde_param[i].encType = 1;
        encoderset->recorde_param[i].mirror = 1;
        encoderset->recorde_param[i].flip = 1;
        encoderset->recorde_param[i].encoder_format = 0;
        encoderset->recorde_param[i].enable_gps_overly = 0;
        encoderset->recorde_param[i].enable_plate_overly = 0;
        encoderset->recorde_param[i].enable_string_overly = 0;
        encoderset->recorde_param[i].enable_time_overly = 0;

        encoderset->recorde_param[i + 4].timeoverly_pos.pos_x = 66;
        encoderset->recorde_param[i + 4].timeoverly_pos.pos_y = 10;

        encoderset->recorde_param[i + 4].stringoverly_pos.pos_x = 10;
        encoderset->recorde_param[i + 4].stringoverly_pos.pos_y = 10;

        encoderset->recorde_param[i + 4].gpsoverly_pos.pos_x = 10; //
        encoderset->recorde_param[i + 4].gpsoverly_pos.pos_y = 88; //

        encoderset->recorde_param[i + 4].plateoverly_pos.pos_x = 10; //
        encoderset->recorde_param[i + 4].plateoverly_pos.pos_y = 80; //

        encoderset->recorde_param[i + 4].encoder_size = ENCODER_CIF;
        encoderset->recorde_param[i + 4].bitstreame_control = DYNAMIC_BITSTREAM;
        encoderset->recorde_param[i + 4].bitstreame_quality = StreamQuality_BEST;
        encoderset->recorde_param[i + 4].bitstreame_size = 1;
        encoderset->recorde_param[i + 4].framerate = 7;
        encoderset->recorde_param[i + 4].encType = 1;
        encoderset->recorde_param[i + 4].mirror = 1;
        encoderset->recorde_param[i + 4].flip = 1;
        encoderset->recorde_param[i + 4].encoder_format = encoderset->recorde_param[i].encoder_format;
    }

    encoderset->recorde_param[0].encoder_size = ENCODER_1080P; // 第一通道是1080P
}

void LoadDefaultRecordSet(RecordSet_t *recordset)
{

    memset(recordset, 0, sizeof(RecordSet_t));
    recordset->record_time_length = 30; // 30 min

    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 7; j++)
        {
            // period1
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period1.StartTime.ulTime = 0;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period1.EndTime.TimeFieldDef.Hour = 23;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period1.EndTime.TimeFieldDef.Minute = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period1.EndTime.TimeFieldDef.Second = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].period1_record_type = 1; // 0bit---普通录像	1bit-----动检录像  2bit----报警录像
            // period2
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period2.StartTime.ulTime = 0;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period2.EndTime.TimeFieldDef.Hour = 23;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period2.EndTime.TimeFieldDef.Minute = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period2.EndTime.TimeFieldDef.Second = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].period2_record_type = 1; // 0bit---普通录像	1bit-----动检录像  2bit----报警录像
            // period3
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period3.StartTime.ulTime = 0;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period3.EndTime.TimeFieldDef.Hour = 23;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period3.EndTime.TimeFieldDef.Minute = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period3.EndTime.TimeFieldDef.Second = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].period3_record_type = 1; // 0bit---普通录像	1bit-----动检录像  2bit----报警录像
            // period4
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period4.StartTime.ulTime = 0;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period4.EndTime.TimeFieldDef.Hour = 23;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period4.EndTime.TimeFieldDef.Minute = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].Period4.EndTime.TimeFieldDef.Second = 59;
            recordset->record_paln.record_week_plan[i].recdayplan[j].period4_record_type = 1; // 0bit---普通录像	1bit-----动检录像  2bit----报警录像
        }
    }
}

void LoadDefaultPTZSet(PTZSet_t *ptzset)
{
    memset(ptzset, 0, sizeof(PTZSet_t));

    for (int i = 0; i < 8; i++)
    {
        ptzset->ptzparam[i].BaudRate = BaudRate_9600;
        ptzset->ptzparam[i].DataBits = DataBits_8;
        ptzset->ptzparam[i].StopBits = StopBits_1;
        ptzset->ptzparam[i].StopBits = ParityBits_None;
        ptzset->ptzparam[i].FlowControl = FlowControl_None;
        ptzset->ptzparam[i].ProtocolIdx = 0;
        ptzset->ptzparam[i].Address = 0;
        ptzset->ptzparam[i].Speed = (0x3f + 1) / 2;
    }
}

void LoadDefaultNetworkParam(NetworkParam_t *networkparam)
{
    memset(networkparam, 0, sizeof(NetworkParam_t));
    networkparam->NetIPAddr.uiIPAddr = 0xc0 << 24 + 0xa8 << 16 + 0 << 8 + 0x6a; // QHostAddress("192.168.0.106").toIPv4Address();
    networkparam->NetIPAddr.szIPAddr.ucData1 = 192;
    networkparam->NetIPAddr.szIPAddr.ucData2 = 168;
    networkparam->NetIPAddr.szIPAddr.ucData3 = 0;
    networkparam->NetIPAddr.szIPAddr.ucData4 = 106;

    networkparam->NetMask.uiIPAddr = 0xff << 24 + 0xff << 16 + 0xff << 8 + 0; // QHostAddress("255.255.255.0").toIPv4Address();
    networkparam->NetMask.szIPAddr.ucData1 = 255;
    networkparam->NetMask.szIPAddr.ucData2 = 255;
    networkparam->NetMask.szIPAddr.ucData3 = 255;
    networkparam->NetMask.szIPAddr.ucData4 = 0;

    networkparam->NetGateway.uiIPAddr = 0xc0 << 24 + 0xa8 << 16 + 0 << 8 + 0x01; // QHostAddress("192.168.0.1").toIPv4Address();
    networkparam->NetGateway.szIPAddr.ucData1 = 192;
    networkparam->NetGateway.szIPAddr.ucData2 = 168;
    networkparam->NetGateway.szIPAddr.ucData3 = 0;
    networkparam->NetGateway.szIPAddr.ucData4 = 1;

    networkparam->NetPriDNSIP.uiIPAddr = 0x08 << 24 + 0x08 << 16 + 0x08 << 8 + 0x08; // QHostAddress("8.8.8.8").toIPv4Address();
    networkparam->NetPriDNSIP.szIPAddr.ucData1 = 8;
    networkparam->NetPriDNSIP.szIPAddr.ucData2 = 8;
    networkparam->NetPriDNSIP.szIPAddr.ucData3 = 8;
    networkparam->NetPriDNSIP.szIPAddr.ucData4 = 8;

    networkparam->PPOEIP.uiIPAddr = 0; // QHostAddress("0.0.0.0").toIPv4Address();
    networkparam->PPOEIP.szIPAddr.ucData1 = 0;
    networkparam->PPOEIP.szIPAddr.ucData2 = 0;
    networkparam->PPOEIP.szIPAddr.ucData3 = 0;
    networkparam->PPOEIP.szIPAddr.ucData4 = 0;

    networkparam->cmsparam.CMSIPAdress.uiIPAddr = 0xc6 << 24 + 0x0b << 16 + 0xb5 << 8 + 0x85; // QHostAddress("198.11.181.133").toIPv4Address();//GovernmentCMSIPAdress
    networkparam->cmsparam.CMSIPAdress.szIPAddr.ucData1 = 198;
    networkparam->cmsparam.CMSIPAdress.szIPAddr.ucData2 = 11;
    networkparam->cmsparam.CMSIPAdress.szIPAddr.ucData3 = 181;
    networkparam->cmsparam.CMSIPAdress.szIPAddr.ucData4 = 133;

    networkparam->cmsparam.CMSType = 0; // SensorType

    sprintf(networkparam->cmsparam.CMSDomainName, "www.xxxx.com"); // GovernmentCMSDomainName
    sprintf(networkparam->cmsparam.CMSPort, "6608");               // GovernmentCMSPort

    sprintf(networkparam->server_port, "7777");
    sprintf(networkparam->http_port, "6666");
    sprintf(networkparam->mobilephone_port, "80");
    sprintf(networkparam->mobilephone_port, "8888");

    memset(&networkparam->upnp, 0, sizeof(networkparam->upnp));
}

void LoadDefaultAlarmSet(AlarmSet_t *alarmset)
{

    memset(alarmset, 0, sizeof(AlarmSet_t));

    for (int i = 0; i < 8; i++)
    {
        alarmset->alarmplan[i].alarm_delay = 10;

        for (int j = 0; j < 7; j++)
        {
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period1.StartTime.ulTime = 0;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period1.EndTime.TimeFieldDef.Hour = 23;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period1.EndTime.TimeFieldDef.Minute = 59;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period1.EndTime.TimeFieldDef.Second = 59;

            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period2.StartTime.ulTime = 0;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period2.EndTime.TimeFieldDef.Hour = 23;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period2.EndTime.TimeFieldDef.Minute = 59;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].period2.EndTime.TimeFieldDef.Second = 59;

            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].enable_period1_alarm_output = 1;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].enable_period1_screen_dispaly = 1;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].enable_period2_alarm_output = 1;
            alarmset->alarmplan[i].alarmweekplan.alarmdayplan[j].enable_period2_screen_dispaly = 1;
        }
    }
}

void LoadDefaultVideoDetectSet(VideoDetectSet_t *videodetectset)
{
    memset(videodetectset, 0, sizeof(VideoDetectSet_t));

    for (int i = 0; i < 8; i++)
    {
        videodetectset->motion_detect[i].detect_delay = 5;
        videodetectset->motion_detect[i].montion_sensitivity = SENSITIVITY_HIGHGENERAL;

        for (int j = 0; j < 7; j++)
        {
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period1.StartTime.ulTime = 0;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period1.EndTime.TimeFieldDef.Hour = 23;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period1.EndTime.TimeFieldDef.Minute = 59;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period1.EndTime.TimeFieldDef.Second = 59;

            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period2.StartTime.ulTime = 0;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period2.EndTime.TimeFieldDef.Hour = 23;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period2.EndTime.TimeFieldDef.Minute = 59;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].period2.EndTime.TimeFieldDef.Second = 59;

            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].enable_period1_alarm_output = 1;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].enable_period1_screen_dispaly = 1;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].enable_period2_alarm_output = 1;
            videodetectset->motion_detect[i].detectweekplan.motionDayplan[j].enable_period2_screen_dispaly = 1;
        }
    }

    for (int i = 0; i < 8; i++)
    {
        videodetectset->videolost_detect[i].detect_delay = 5;
        videodetectset->videolost_detect[i].montion_sensitivity = SENSITIVITY_HIGHGENERAL;

        for (int j = 0; j < 7; j++)
        {
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period1.StartTime.ulTime = 0;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period1.EndTime.TimeFieldDef.Hour = 23;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period1.EndTime.TimeFieldDef.Minute = 59;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period1.EndTime.TimeFieldDef.Second = 59;

            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period2.StartTime.ulTime = 0;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period2.EndTime.TimeFieldDef.Hour = 23;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period2.EndTime.TimeFieldDef.Minute = 59;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].period2.EndTime.TimeFieldDef.Second = 59;

            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].enable_period2_alarm_output = 1;
            videodetectset->videolost_detect[i].detectweekplan.motionDayplan[j].enable_period2_screen_dispaly = 1;
        }
    }
}

void LoadDefaultLocalDisplay(LocalDisplay_T *localdisplayset)
{

    memset(localdisplayset, 0, sizeof(LocalDisplay_T));

    localdisplayset->default_layout = 0;

    for (int i = 0; i < 8; i++)
    {

        sprintf(localdisplayset->channel_name[i], "CH %02d", i + 1);
    }
}

void LoadDefaultMSServerConfig(MSServerConfig_t *msserverconfig)
{
    memset(msserverconfig, 0, sizeof(MSServerConfig_t));

    msserverconfig->heart_timeinterval = 180;
    msserverconfig->gps_timeinterval = 30;
    msserverconfig->timeout_resend = 30;
}

void LoadDefaultAutoMangeParam(AutoMangeParam_t *automangeset)
{
    sprintf(automangeset->BasicInfo.VehicleNum, "vehicle");
    sprintf(automangeset->BasicInfo.CompanyName, "companyname");
    sprintf(automangeset->BasicInfo.LicenseNum, "license");
    sprintf(automangeset->BasicInfo.DriverName, "drivername");
    sprintf(automangeset->BasicInfo.LineNum, "lineNum");

    automangeset->AccSet.Thresholdx = 1;
    automangeset->AccSet.Thresholdy = 1;
    automangeset->AccSet.Thresholdz = 1;

    automangeset->AccSet.Enablex = 1;
    automangeset->AccSet.Enabley = 1;
    automangeset->AccSet.Enablez = 1;

    automangeset->AccSet.Videox = 1;
    automangeset->AccSet.Videoy = 1;
    automangeset->AccSet.Videoz = 1;

    automangeset->AccSet.Offsetx = 0;
    automangeset->AccSet.Offsety = 0;
    automangeset->AccSet.Offsetz = 0;

    automangeset->WirelessSet.WirelessEnable = 0;
    automangeset->WirelessSet.WirelessType = 0;
    sprintf(automangeset->WirelessSet.DialNum, "*99#");
    sprintf(automangeset->WirelessSet.LogPWD, "cmnet");
    sprintf(automangeset->WirelessSet.LogUser, "cmnet");
    sprintf(automangeset->WirelessSet.WirelessAccess, "3gnet");
    automangeset->WirelessSet.SignalStrength = 99;
    automangeset->WirelessSet.WirelessNetGateway.uiIPAddr = 0;                                    // QHostAddress("0.0.0.0").toIPv4Address();
    automangeset->WirelessSet.WirelessNetIPAddr.uiIPAddr = 0;                                     // QHostAddress("0.0.0.0").toIPv4Address();
    automangeset->WirelessSet.WirelessNetMask.uiIPAddr = 0xff << 24 + 0xff << 16 + 0xff << 8 + 0; // QHostAddress("255.255.255.0").toIPv4Address();

    automangeset->WifiSet.WifiEnable = 0;
    automangeset->WifiSet.WifiNetGateway.uiIPAddr = 0xc0 << 24 + 0xa8 << 16 + 0 << 8 + 0x01; // QHostAddress("192.168.0.1").toIPv4Address();
    automangeset->WifiSet.WifiNetGateway.szIPAddr.ucData1 = 192;
    automangeset->WifiSet.WifiNetGateway.szIPAddr.ucData2 = 168;
    automangeset->WifiSet.WifiNetGateway.szIPAddr.ucData3 = 0;
    automangeset->WifiSet.WifiNetGateway.szIPAddr.ucData4 = 1;

    automangeset->WifiSet.WifiNetIPAddr.uiIPAddr = 0xc0 << 24 + 0xa8 << 16 + 0 << 8 + 0x6a; // QHostAddress("192.168.0.106").toIPv4Address();
    automangeset->WifiSet.WifiNetIPAddr.szIPAddr.ucData1 = 192;
    automangeset->WifiSet.WifiNetIPAddr.szIPAddr.ucData2 = 168;
    automangeset->WifiSet.WifiNetIPAddr.szIPAddr.ucData3 = 0;
    automangeset->WifiSet.WifiNetIPAddr.szIPAddr.ucData4 = 106;
    automangeset->WifiSet.WifiNetMask.uiIPAddr = 0xff << 24 + 0xff << 16 + 0xff << 8 + 0; // QHostAddress("255.255.255.0").toIPv4Address();
    automangeset->WifiSet.WifiNetMask.szIPAddr.ucData1 = 255;
    automangeset->WifiSet.WifiNetMask.szIPAddr.ucData2 = 255;
    automangeset->WifiSet.WifiNetMask.szIPAddr.ucData3 = 255;
    automangeset->WifiSet.WifiNetMask.szIPAddr.ucData4 = 0;
    automangeset->WifiSet.WifiNetworkMode = 0;

    for (int i = 0; i < 5; i++)
    {
        // sprintf(automangeset->WifiSet.WifiHotspot[i].WifiEssid,"");
        // sprintf(automangeset->WifiSet.WifiHotspot[i].WifiPWD,"");
        memset(&automangeset->WifiSet.WifiHotspot[i].WifiEssid, 0, 32);
        memset(&automangeset->WifiSet.WifiHotspot[i].WifiPWD, 0, 32);
    }

    automangeset->MDvrSet.IsOpenOrClose = 0;
    automangeset->MDvrSet.DelayOpenOrClose = 0;
    automangeset->MDvrSet.DelayClosetime = 0;
    automangeset->MDvrSet.Opentime[0] = 0x0c;
    automangeset->MDvrSet.Opentime[1] = 0x00;
    automangeset->MDvrSet.Opentime[2] = 0x00;

    automangeset->MDvrSet.Closetime[0] = 0x0c;
    automangeset->MDvrSet.Closetime[1] = 0x00;
    automangeset->MDvrSet.Closetime[2] = 0x00;
}

void LoadDefaultCameralSetting(CameralSetting_t *cameraset)
{
    memset(cameraset, 0, sizeof(CameralSetting_t));
    cameraset->chtoshow[0] = 0x01;
    cameraset->chtoshow[1] = 0x02;
    cameraset->chtoshow[2] = 0x04;
    cameraset->chtoshow[3] = 0x08;
    cameraset->chtoshow[4] = 0x10;
    cameraset->chtoshow[5] = 0x20;
    cameraset->chtoshow[6] = 0x40;
    cameraset->chtoshow[7] = 0x80;
    cameraset->gridtoshow[1] = 0x02;
    cameraset->delayoff[1] = 0;
    cameraset->delayoff[2] = 5;
    cameraset->delayoff[0] = 5;
    cameraset->delayoff[6] = 5;
    cameraset->delayoff[7] = 5;
    cameraset->delayoff[8] = 5;
    cameraset->delayoff[9] = 5;
}

void LoadDefaultBacklineStruct(BacklineStruct_t *backlinepointdata)
{
    BacklineStruct_t *backlineData = backlinepointdata;
    float screenwidth = 1024.0;

    backlineData->backlinespace = 30;
    backlineData->backlinewidth = 40.0;
    backlineData->backlineheight = 128.0;
    backlineData->backlinedefaultposx = 115.0;
    backlineData->backlinedefaultposy = 0.0;
    backlineData->penlinewidth = 10;

    // red
    backlineData->leftredbottom[0] = screenwidth - backlineData->backlinedefaultposx; // x
    backlineData->leftredbottom[1] = backlineData->backlinedefaultposy;               // y
    backlineData->leftredtop[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftredtop[1] = backlineData->backlineheight;                                                      // 128.0;
    backlineData->leftredright[0] = screenwidth - (backlineData->backlinedefaultposx + backlineData->backlinewidth); // 195.0;
    backlineData->leftredright[1] = backlineData->backlineheight;                                                    // 128.0;

    // blue
    backlineData->leftbluebottom[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftbluebottom[1] = backlineData->backlineheight + backlineData->backlinespace; // 128.0+backlineData->backlinespace;
    backlineData->leftbluetop[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftbluetop[1] = backlineData->backlineheight * 2 + backlineData->backlinespace;                    // 256.0+backlineData->backlinespace;
    backlineData->leftblueright[0] = screenwidth - (backlineData->backlinedefaultposx + backlineData->backlinewidth); // 195.0;
    backlineData->leftblueright[1] = backlineData->backlineheight * 2 + backlineData->backlinespace;                  // 256.0+backlineData->backlinespace;

    // yellow
    backlineData->leftyellowbottom[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftyellowbottom[1] = backlineData->backlineheight * 2 + backlineData->backlinespace * 2; // 256.0+backlineData->backlinespace*2;
    backlineData->leftyellowtop[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftyellowtop[1] = backlineData->backlineheight * 3 + backlineData->backlinespace * 2;                // 384.0+backlineData->backlinespace*2;
    backlineData->leftyellowright[0] = screenwidth - (backlineData->backlinedefaultposx + backlineData->backlinewidth); // 195.0;
    backlineData->leftyellowright[1] = backlineData->backlineheight * 3 + backlineData->backlinespace * 2;              // 384.0+backlineData->backlinespace*2;

    // green
    backlineData->leftgreenbottom[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftgreenbottom[1] = backlineData->backlineheight * 3 + backlineData->backlinespace * 3; // 384.0+backlineData->backlinespace*3;
    backlineData->leftgreentop[0] = screenwidth - backlineData->backlinedefaultposx;
    backlineData->leftgreentop[1] = backlineData->backlineheight * 4 + backlineData->backlinespace * 3;                // 512.0+backlineData->backlinespace*3;
    backlineData->leftgreenright[0] = screenwidth - (backlineData->backlinedefaultposx + backlineData->backlinewidth); // 195.0;
    backlineData->leftgreenright[1] = backlineData->backlineheight * 4 + backlineData->backlinespace * 3;              // 512.0+backlineData->backlinespace*3;
    /////////////////////////////////////////////////////////////////////////

    backlineData->rightredbottom[0] = backlineData->backlinedefaultposx;
    backlineData->rightredbottom[1] = backlineData->backlinedefaultposy; // y
    backlineData->rightredtop[0] = backlineData->backlinedefaultposx;
    backlineData->rightredtop[1] = backlineData->backlineheight;                                       // 128.0;
    backlineData->rightredleft[0] = (backlineData->backlinedefaultposx + backlineData->backlinewidth); // 195.0;
    backlineData->rightredleft[1] = backlineData->backlineheight;                                      // 128.0;

    backlineData->rightbluebottom[0] = backlineData->backlinedefaultposx;
    backlineData->rightbluebottom[1] = backlineData->backlineheight + backlineData->backlinespace; // 128.0+backlineData->backlinespace;//y
    backlineData->rightbluetop[0] = backlineData->backlinedefaultposx;
    backlineData->rightbluetop[1] = backlineData->backlineheight * 2 + backlineData->backlinespace;     // 256.0+backlineData->backlinespace;
    backlineData->rightblueleft[0] = (backlineData->backlinedefaultposx + backlineData->backlinewidth); // 195.0;
    backlineData->rightblueleft[1] = backlineData->backlineheight * 2 + backlineData->backlinespace;    // 256.0+backlineData->backlinespace;

    backlineData->rightyellowbottom[0] = backlineData->backlinedefaultposx;
    backlineData->rightyellowbottom[1] = backlineData->backlineheight * 2 + backlineData->backlinespace * 2; // 256.0+backlineData->backlinespace*2; //y
    backlineData->rightyellowtop[0] = backlineData->backlinedefaultposx;
    backlineData->rightyellowtop[1] = backlineData->backlineheight * 3 + backlineData->backlinespace * 2;  // 384.0+backlineData->backlinespace*2;
    backlineData->rightyellowleft[0] = (backlineData->backlinedefaultposx + backlineData->backlinewidth);  // 195.0;
    backlineData->rightyellowleft[1] = backlineData->backlineheight * 3 + backlineData->backlinespace * 2; // 384.0+backlineData->backlinespace*2;

    backlineData->rightgreenbottom[0] = backlineData->backlinedefaultposx;
    backlineData->rightgreenbottom[1] = backlineData->backlineheight * 3 + backlineData->backlinespace * 3; // 384.0+backlineData->backlinespace*3; //y
    backlineData->rightgreentop[0] = backlineData->backlinedefaultposx;
    backlineData->rightgreentop[1] = backlineData->backlineheight * 4 + backlineData->backlinespace * 3;  // 512.0+backlineData->backlinespace*3;
    backlineData->rightgreenleft[0] = (backlineData->backlinedefaultposx + backlineData->backlinewidth);  // 195.0;
    backlineData->rightgreenleft[1] = backlineData->backlineheight * 4 + backlineData->backlinespace * 3; // 512.0+backlineData->backlinespace*3;
}

void LoadDefaultABDParam(ABDParam_t *abdparam)
{

    abdparam->AlgorithmSelect.DMS_Enable = 1;
    abdparam->AlgorithmSelect.ADAS_Enable = 1;
    int bsdcount = abdparam->IdaCfgSet.bsdConfig.BSDCount;
    bool dms = abdparam->AlgorithmSelect.DMS_Enable;
    bool adas = abdparam->AlgorithmSelect.ADAS_Enable;
    bool bsd = abdparam->AlgorithmSelect.BSD_Enable;
    abdparam->algVideoChnSelect.DMS_VideoChn_num = 1;
    AlgVideoChnSelect_S algCHnSelect;
    memcpy(&algCHnSelect, &abdparam->algVideoChnSelect, sizeof(AlgVideoChnSelect_S));

    memset(abdparam, 0, sizeof(ABDParam_t));
    abdparam->IdaCfgSet.RecordStreamConfig.enableMainstream = 1;  // 主码流录像使能：0=关闭，1=开启
    abdparam->IdaCfgSet.RecordStreamConfig.enableSubstream = 1;   // 子码流录像使能：0=关闭，1=开启
    abdparam->IdaCfgSet.RecordStreamConfig.enableThirdstream = 1; // 第三码流录像使能：0=关闭，1=开启

    abdparam->IdaCfgSet.StorageConfig.fileNum = 10;      // 文件数量
    abdparam->IdaCfgSet.StorageConfig.fileDuration = 10; // 文件持续时间(MB)
    abdparam->IdaCfgSet.StorageConfig.diskSpace = 100;   // 磁盘容量
    abdparam->IdaCfgSet.StorageConfig.diskStatus = 1;    // 磁盘运行状态

    abdparam->IdaCfgSet.runstatus.basicStatus_t.cpuUsage = 15.5;
    abdparam->IdaCfgSet.runstatus.basicStatus_t.memoryUsage = 14.4;
    abdparam->IdaCfgSet.runstatus.basicStatus_t.storageUsage = 14.4;
    abdparam->IdaCfgSet.runstatus.basicStatus_t.tfCardUsage = 14.4;
    abdparam->IdaCfgSet.runstatus.basicStatus_t.tfCardWriteNormal = true;            // TF卡写入正常
    abdparam->IdaCfgSet.runstatus.basicStatus_t.storageCapacityNormal = true;        // 存储容量正常
    abdparam->IdaCfgSet.runstatus.basicStatus_t.storageSpaceNormal = true;           // 存储空间状态正常
    abdparam->IdaCfgSet.runstatus.basicStatus_t.tfCardFormatNormal = true;           // TF卡格式正常
    strcpy(abdparam->IdaCfgSet.runstatus.basicStatus_t.date, "2024-06-18 15:28:05"); // 日期
    strcpy(abdparam->IdaCfgSet.runstatus.basicStatus_t.runTime, "6:00:00");          // 固定运行时间
    abdparam->IdaCfgSet.runstatus.basicStatus_t.temperature = 11.1;

    strcpy(abdparam->IdaCfgSet.runstatus.mobileNetworkStatus_t.imei, "000000000000000"); // IMEI号
    abdparam->IdaCfgSet.runstatus.mobileNetworkStatus_t.networkType = 0;                 // 网络类型: 4G
    abdparam->IdaCfgSet.runstatus.mobileNetworkStatus_t.signalStrength = 10;             // 信号强度(dBm)
    abdparam->IdaCfgSet.runstatus.mobileNetworkStatus_t.networkConnected = true;         // 网络已连接

    abdparam->IdaCfgSet.runstatus.dvrStatus_t.channel1Normal = 1; // 通道1是否正常
    abdparam->IdaCfgSet.runstatus.dvrStatus_t.channel2Normal = 1; // 通道2是否正常
    abdparam->IdaCfgSet.runstatus.dvrStatus_t.channel3Normal = 1; // 通道3是否正常
    abdparam->IdaCfgSet.runstatus.dvrStatus_t.channel4Normal = 1; // 通道4是否正常

    abdparam->IdaCfgSet.runstatus.gpsStatus_t.antennaConnected = 1; // 天线状态: true=已连接, false=断开
    abdparam->IdaCfgSet.runstatus.gpsStatus_t.positionStatus = 0;   // 位置状态: 0=未定位, 1=2D定位, 2=3D定位
    abdparam->IdaCfgSet.runstatus.gpsStatus_t.satelliteCount = 1;   // 卫星数量
    abdparam->IdaCfgSet.runstatus.gpsStatus_t.speed = 10.5;         // 速度

    strcpy(abdparam->IdaCfgSet.productinfo.basicInfo.model, "MSC807RK");       // 设备型号
    strcpy(abdparam->IdaCfgSet.productinfo.basicInfo.appVersion, "V100");      // 应用程序版本
    strcpy(abdparam->IdaCfgSet.productinfo.basicInfo.firmwareVersion, "V200"); // 固件版本

    strcpy(abdparam->IdaCfgSet.productinfo.networkInfo.iccid, "120"); // ICCID
    strcpy(abdparam->IdaCfgSet.productinfo.networkInfo.imei, "120");  // IMEI
    strcpy(abdparam->IdaCfgSet.productinfo.networkInfo.modem, "120"); // 4G调制解调器标识

    // 夏令时配置
    abdparam->IdaCfgSet.devicecfg.daylightSaving.enabled = 0; // 启用夏令时
    abdparam->IdaCfgSet.devicecfg.daylightSaving.useUTC = 1;  // 不基于UTC

    abdparam->IdaCfgSet.devicecfg.daylightSaving.startTime.year = 0;        // 每年
    abdparam->IdaCfgSet.devicecfg.daylightSaving.startTime.month = 3;       // 3月
    abdparam->IdaCfgSet.devicecfg.daylightSaving.startTime.weekOfMonth = 2; // 第二个星期
    abdparam->IdaCfgSet.devicecfg.daylightSaving.startTime.dayOfWeek = 0;   // 星期日
    abdparam->IdaCfgSet.devicecfg.daylightSaving.startTime.hour = 2;        // 2点

    abdparam->IdaCfgSet.devicecfg.daylightSaving.endTime.year = 0;        // 每年
    abdparam->IdaCfgSet.devicecfg.daylightSaving.endTime.month = 10;      // 10月
    abdparam->IdaCfgSet.devicecfg.daylightSaving.endTime.weekOfMonth = 1; // 第一个星期
    abdparam->IdaCfgSet.devicecfg.daylightSaving.endTime.dayOfWeek = 0;   // 星期日
    abdparam->IdaCfgSet.devicecfg.daylightSaving.endTime.hour = 2;        // 2点

    // 远程平台配置
    strcpy(abdparam->IdaCfgSet.devicecfg.remotePlatform.ip, "192.168.11.104");
    abdparam->IdaCfgSet.devicecfg.remotePlatform.port = 80;
    abdparam->IdaCfgSet.devicecfg.remotePlatform.isConnected = 1;

    // 服务器平台配置
    strcpy(abdparam->IdaCfgSet.devicecfg.serverPlatform.ip, "47.102.137.223");
    abdparam->IdaCfgSet.devicecfg.serverPlatform.port = 8808;
    strcpy(abdparam->IdaCfgSet.devicecfg.serverPlatform.id, "1111");
    abdparam->IdaCfgSet.devicecfg.serverPlatform.isConnected = 1;
    strcpy(abdparam->IdaCfgSet.devicecfg.serverPlatform.plateNumber, "1111");
    strcpy(abdparam->IdaCfgSet.devicecfg.serverPlatform.plateColor, "蓝");
    abdparam->IdaCfgSet.devicecfg.serverPlatform.immediateReportEnabled = 1;

    // 基本配置
    strcpy(abdparam->IdaCfgSet.devicecfg.basicConfig.timeZoneCode, "GMT+0:00");
    strcpy(abdparam->IdaCfgSet.devicecfg.basicConfig.timeZoneName, "阿比让/非洲");
    strcpy(abdparam->IdaCfgSet.devicecfg.basicConfig.timeZoneId, "Abidjan(Africa)");
    abdparam->IdaCfgSet.devicecfg.basicConfig.language = 1; // 中文

    // 实时视频配置
    abdparam->IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[0] = 1;
    abdparam->IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[1] = 1;
    abdparam->IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[2] = 0;
    abdparam->IdaCfgSet.devicecfg.realTimeVideo.channelEnabled[3] = 0;

    // AHD配置
    abdparam->IdaCfgSet.devicecfg.ahdConfig.channelEnabled[0] = false; // 通道1
    abdparam->IdaCfgSet.devicecfg.ahdConfig.channelEnabled[1] = false; // 通道2
    abdparam->IdaCfgSet.devicecfg.ahdConfig.channelEnabled[2] = false; // 通道3
    abdparam->IdaCfgSet.devicecfg.ahdConfig.channelEnabled[3] = false; // 通道4
    abdparam->IdaCfgSet.devicecfg.ahdConfig.activeChannel = 4;

    abdparam->IdaCfgSet.drivlist.count = 0;
    // abdparam->IdaCfgSet.drivlist.drivers = (DriverInfo *)malloc(abdparam->IdaCfgSet.drivlist.count * sizeof(DriverInfo));
    // for (int i = 0; i < abdparam->IdaCfgSet.drivlist.count; ++i)
    // {
    //     strcpy(abdparam->IdaCfgSet.drivlist.drivers[i].driverId, "25648");
    //     strcpy(abdparam->IdaCfgSet.drivlist.drivers[i].driverName, "南苏丹");
    //     //strcpy(abdparam->IdaCfgSet.drivlist.drivers[i].photoPath, "/userdata/driver_photos/123.jpg");
    //     abdparam->IdaCfgSet.drivlist.drivers[i].hasPhoto = true;
    //     abdparam->IdaCfgSet.drivlist.drivers[i].registerTime = time(NULL) - 3600;
    //     abdparam->IdaCfgSet.drivlist.drivers[i].isActive = true;
    //     strcpy(abdparam->IdaCfgSet.drivlist.drivers[i].photoId, "111");
    // }
    // strcpy(abdparam->IdaCfgSet.drivlist.drivers[1].driverId, "25648");
    // strcpy(abdparam->IdaCfgSet.drivlist.drivers[1].driverName, "南苏丹");
    // strcpy(abdparam->IdaCfgSet.drivlist.drivers[1].photoPath, "/userdata/driver_photos/123.jpg");
    // abdparam->IdaCfgSet.drivlist.drivers[1].hasPhoto = false;
    // abdparam->IdaCfgSet.drivlist.drivers[1].registerTime = time(NULL) - 7200;  // 晚一些
    // abdparam->IdaCfgSet.drivlist.drivers[1].isActive = true;
    // strcpy(abdparam->IdaCfgSet.drivlist.drivers[1].photoId, "111");
    // free(abdparam->IdaCfgSet.drivlist.drivers);

    // abdparam->IdaCfgSet.verirecordlist.count = 1;
    // abdparam->IdaCfgSet.verirecordlist.records = (VerifyRecord *)malloc(abdparam->IdaCfgSet.verirecordlist.count * sizeof(VerifyRecord));
    // for (int i = 0; i < abdparam->IdaCfgSet.verirecordlist.count; ++i)
    // {
    //     strcpy(abdparam->IdaCfgSet.verirecordlist.records[i].driverId, "25650");
    //     strcpy(abdparam->IdaCfgSet.verirecordlist.records[i].driverName, "南苏丹");
    //     strcpy(abdparam->IdaCfgSet.verirecordlist.records[i].photoPath, "/userdata/driver_photos/123.jpg");
    //     abdparam->IdaCfgSet.verirecordlist.records[i].verifyTime = time(NULL) - 3600;
    //     abdparam->IdaCfgSet.verirecordlist.records[i].verifyType = VERIFY_TYPE_IDENTITY_t;    // 0
    //     abdparam->IdaCfgSet.verirecordlist.records[i].verifyResult = VERIFY_RESULT_SUCCESS_t; // 0
    // }

    // strcpy(abdparam->IdaCfgSet.verirecordlist.records[1].driverId, "25648");
    // strcpy(abdparam->IdaCfgSet.verirecordlist.records[1].driverName, "南苏丹");
    // strcpy(abdparam->IdaCfgSet.verirecordlist.records[1].photoPath, "/userdata/driver_photos/124.jpg");
    // abdparam->IdaCfgSet.verirecordlist.records[1].verifyTime = time(NULL) - 7200;
    // abdparam->IdaCfgSet.verirecordlist.records[1].verifyType = VERIFY_TYPE_REGISTER_t; // 1
    // abdparam->IdaCfgSet.verirecordlist.records[1].verifyResult = VERIFY_RESULT_SUCCESS_t; // 0
    // free(abdparam->IdaCfgSet.verirecordlist.records);

    abdparam->IdaCfgSet.captuconfig.enabled = 1;   // 是否启用抓拍功能
    abdparam->IdaCfgSet.captuconfig.interval = 10; // 抓拍间隔时间(秒)

    abdparam->IdaCfgSet.recqueparams.channel = 0;
    abdparam->IdaCfgSet.recqueparams.recordType = 1;
    abdparam->IdaCfgSet.recqueparams.startTime = 1704067200;
    abdparam->IdaCfgSet.recqueparams.endTime = 1704070800;
    abdparam->IdaCfgSet.recqueparams.maxNumPerPage = 10;
    abdparam->IdaCfgSet.recqueparams.currentPage = 1;
    abdparam->IdaCfgSet.recqueparams.bufferLength = 4096;

    abdparam->IdaCfgSet.debugfeaturecfg.runMode.hasLogFile = true; // 日志文件是否存在 1=有 0=无
    abdparam->IdaCfgSet.debugfeaturecfg.runMode.systemVolume = (allConfig.AllConfigDef.advanceset.outputadjust.output_volume * 100) / 15;
    ;                                                                          // 系统音量 0-100
    abdparam->IdaCfgSet.debugfeaturecfg.runMode.driverBootPromptInterval = 5;  // 驾驶员开机提示间隔（秒）
    abdparam->IdaCfgSet.debugfeaturecfg.runMode.isTfCardCheckVoiceOn = false;  // TF卡状态检查语音 0=关 1=开
    abdparam->IdaCfgSet.debugfeaturecfg.runMode.tfCardCheckVoiceInterval = 10; // TF卡状态检查语音间隔（秒）

    // SystemDebugModeConfig_t
    abdparam->IdaCfgSet.debugfeaturecfg.debugMode.isVoiceInit = true;  // 是否初始化音频 0=否 1=是
    abdparam->IdaCfgSet.debugfeaturecfg.debugMode.isDebugMode = false; // 系统调试模式 0=关 1=开
    abdparam->IdaCfgSet.debugfeaturecfg.debugMode.mockSpeed = 42;      // 模拟速度设置（km/h）

    // NetworkConfig_t
    abdparam->IdaCfgSet.debugfeaturecfg.network.wirelessHotspotEnabled = true; // 无线热点网开关
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.wifiConfig.ssid, "MyTestWiFi");
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.wifiConfig.password, "12345678");
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.name, "internet");         // APN名称
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.username, "user1");        // 用户名
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.password, "pass1");        // 密码
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.mcc, "460");               // 移动国家代码(MCC)
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.mnc, "00");                // 移动网络代码(MNC)
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.server, "apn.server.com"); // 服务器
    strcpy(abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.proxy, "192.168.1.1");     // 代理
    abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.authType = APN_AUTH_PAP_CHAP_t;   // PAP或CHAP认证
    abdparam->IdaCfgSet.debugfeaturecfg.network.apnConfig.enabled = true;                   // 启用状态

    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].enabled = true;          // 开关
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].channel = 1;             // 通道号 1-4
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].speedThreshold = 60;     // 超速报警阈值,(km/h）
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].preWarningInterval = 10; // 预警间隔, 单位s
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].preWarningSpeed = 55;    // 预警阈值,(km/h）
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].persistenceDuration = 5; //  超速持续时长, 单位s
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].warningInterval = 15;    // 报警间隔, 单位s
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].photoCount = 2;          // 拍照数量
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].photoInterval = 1;       // 拍照间隔,单位s
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[0].videoRecordSeconds = 10; // 报警录制视频秒数,单位s

    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].enabled = false;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].channel = 2;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].speedThreshold = 80;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].preWarningInterval = 12;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].preWarningSpeed = 70;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].persistenceDuration = 8;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].warningInterval = 16;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].photoCount = 3;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].photoInterval = 2;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[1].videoRecordSeconds = 12;

    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].enabled = false;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].channel = 3;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].speedThreshold = 80;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].preWarningInterval = 12;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].preWarningSpeed = 70;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].persistenceDuration = 8;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].warningInterval = 16;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].photoCount = 3;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].photoInterval = 2;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[2].videoRecordSeconds = 12;

    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].enabled = false;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].channel = 4;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].speedThreshold = 80;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].preWarningInterval = 12;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].preWarningSpeed = 70;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].persistenceDuration = 8;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].warningInterval = 16;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].photoCount = 3;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].photoInterval = 2;
    abdparam->IdaCfgSet.debugfeaturecfg.speedWarning.channels[3].videoRecordSeconds = 12;

    // abdparam->IdaCfgSet.capturerecordlist_s.count = 2;
    // abdparam->IdaCfgSet.capturerecordlist_s.records = (CaptureRecord_S *)malloc(sizeof(CaptureRecord_S) * abdparam->IdaCfgSet.capturerecordlist_s.count);
    // for (int i = 0; i < abdparam->IdaCfgSet.capturerecordlist_s.count; ++i)
    // {
    //     strcpy(abdparam->IdaCfgSet.capturerecordlist_s.records[0].driverId, "D001");
    //     strcpy(abdparam->IdaCfgSet.capturerecordlist_s.records[0].driverName, "张三");
    //     strcpy(abdparam->IdaCfgSet.capturerecordlist_s.records[0].photoPath, "/userdata/driver_photos/driver1.jpg");
    //     abdparam->IdaCfgSet.capturerecordlist_s.records[0].captureTime = time(NULL); // 当前时间
    //     abdparam->IdaCfgSet.capturerecordlist_s.records[0].verifyResult = true;

    //     strcpy(abdparam->IdaCfgSet.capturerecordlist_s.records[1].driverId, "D002");
    //     strcpy(abdparam->IdaCfgSet.capturerecordlist_s.records[1].driverName, "李四");
    //     strcpy(abdparam->IdaCfgSet.capturerecordlist_s.records[1].photoPath, "/userdata/driver_photos/driver2.jpg");
    //     abdparam->IdaCfgSet.capturerecordlist_s.records[1].captureTime = time(NULL); // 当前时间
    //     abdparam->IdaCfgSet.capturerecordlist_s.records[1].verifyResult = true;
    // }

    abdparam->AlgorithmSelect.DMS_Enable = dms;
    abdparam->AlgorithmSelect.ADAS_Enable = adas;
    abdparam->AlgorithmSelect.BSD_Enable = bsd;
    memcpy(&abdparam->algVideoChnSelect, &algCHnSelect, sizeof(AlgVideoChnSelect_S));
    abdparam->algVideoChnSelect.Available_BSDChn_num = bsdcount;
    // algVideoChnSelect reset

    // DMS param reset
    abdparam->IdaCfgSet.dmsConfig.calibration = 0;
    abdparam->IdaCfgSet.dmsConfig.alarmType = 0x1FF;
    abdparam->IdaCfgSet.dmsConfig.leftYaw = -30;
    abdparam->IdaCfgSet.dmsConfig.rightYaw = 30;
    abdparam->IdaCfgSet.dmsConfig.upPitch = 25;
    abdparam->IdaCfgSet.dmsConfig.downPitch = -20;
    for (int i = 0; i < 9; i++)
    {
        abdparam->IdaCfgSet.dmsConfig.AlarmParam[i].alarmSensiLevel = 1;
        abdparam->IdaCfgSet.dmsConfig.AlarmParam[i].interval = 5;
        abdparam->IdaCfgSet.dmsConfig.AlarmParam[i].speedThreshold = 30;
    }

    abdparam->IdaCfgSet.dmsConfig.warningParam[0].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[0].secondLevelAlarmSpeed = 60;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[1].secondLevelAlarmSpeed = 60;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[2].secondLevelAlarmSpeed = 60;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[3].secondLevelAlarmSpeed = 60;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[4].secondLevelAlarmSpeed = 60;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[5].secondLevelAlarmSpeed = 60;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].enabled = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].alarmInterval = 100;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].photoNumber = 3;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].sensitivity = 2;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].recordVideoSeconds = 10;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].speedRequirement = 0;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].photoInterval = 200;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].firstLevelAlarmSpeed = 30;
    abdparam->IdaCfgSet.dmsConfig.warningParam[6].secondLevelAlarmSpeed = 60;

    abdparam->IdaCfgSet.dmsConfig.alarmVolume = 0;
    // ADAS param reset
    abdparam->IdaCfgSet.adasConfig.calibration = 0;
    abdparam->IdaCfgSet.adasConfig.alarmType = 0xF;
    abdparam->IdaCfgSet.adasConfig.CalibResult.r1 = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.r2 = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.r3 = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.t1 = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.t2 = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.t3 = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.pitch = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.yaw = 0;
    abdparam->IdaCfgSet.adasConfig.CalibResult.roll = 0;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.horizon = 360 * 1.5;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.carMiddle = 0;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.cameraHeight = 210;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.cameraToAxle = -130;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.carWidth = 230;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.cameraToBumper = 10;
    abdparam->IdaCfgSet.adasConfig.CalibInfo.cameraToLeftWheel = 115;
    for (int i = 0; i < 4; i++)
    {
        if (i == 0)
        {
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].alarmSensiLevel = -0.3;
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].interval = 2;
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].speedThreshold = 50;
        }
        else if (i == 1)
        {
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].alarmSensiLevel = 1;
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].interval = 2;
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].speedThreshold = 1;
        }
        else
        {
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].alarmSensiLevel = 0.5;
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].interval = 5;
            abdparam->IdaCfgSet.adasConfig.AlarmParam[i].speedThreshold = 30;
        }
    }

    for (int i = 0; i < ADAS_WARNING_TYPE_MAX_T; i++)
    {
        abdparam->IdaCfgSet.adasConfig.warningParam[i].enabled = 1;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].alarmInterval = 10;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].photoNumber = 3;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].sensitivity = 2;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].recordVideoSeconds = 10;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].speedRequirement = 0;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].photoInterval = 200;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].firstLevelAlarmSpeed = 30;
        abdparam->IdaCfgSet.adasConfig.warningParam[i].secondLevelAlarmSpeed = 60;
    }

    // BSD param reset
    abdparam->IdaCfgSet.bsdConfig.calibration = 0x00;

    abdparam->IdaCfgSet.bsdConfig.BSDCount = bsdcount;
    for (int i = 0; i < 4; i++)
    {
        if (i == 0)
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position = BSD_CAMERA_POSITION_TYPE_FRONTRIGHT;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].BSDType = 1;
        }
        else if (i == 1)
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position = BSD_CAMERA_POSITION_TYPE_LEFTFRONT;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].BSDType = 0;
        }
        else if (i == 2)
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position = BSD_CAMERA_POSITION_TYPE_RIGHTFRONT;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].BSDType = 0;
        }
        else
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position = BSD_CAMERA_POSITION_TYPE_FRONTRIGHT;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].BSDType = 1;
        }

        if (abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_FORWARD || abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_FRONTLEFT || abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_FRONTRIGHT)
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[0].x = 46;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[0].y = 593;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[1].x = 472;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[1].y = 632;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[2].x = 784;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[2].y = 610;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[3].x = 1255;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[3].y = 527;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[0].x = 22;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[0].y = 419;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[1].x = 1253;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[1].y = 431;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[0].x = 21;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[0].y = 286;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[1].x = 1260;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[1].y = 324;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[0].x = 20;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[0].y = 170;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[1].x = 1253;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[1].y = 222;
        }
        if (abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_RIGHTREAR || abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_LEFTFRONT)
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[0].x = 1080;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[0].y = 80;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[1].x = 1098;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[1].y = 310;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[2].x = 1103;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[2].y = 497;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[3].x = 1092;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[3].y = 700;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[0].x = 1017;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[0].y = 205;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[1].x = 754;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[1].y = 707;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[0].x = 946;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[0].y = 205;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[1].x = 490;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[1].y = 697;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[0].x = 897;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[0].y = 187;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[1].x = 235;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[1].y = 708;
        }
        if (abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_LEFTREAR || abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].position == BSD_CAMERA_POSITION_TYPE_RIGHTFRONT)
        {
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[0].x = 136;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[0].y = 64;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[1].x = 117;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[1].y = 298;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[2].x = 119;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[2].y = 461;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[3].x = 139;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].baseLinePoints[3].y = 699;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[0].x = 184;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[0].y = 138;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[1].x = 441;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].highDangerLine[1].y = 709;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[0].x = 247;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[0].y = 129;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[1].x = 704;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].mediumDangerLine[1].y = 700;

            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[0].x = 312;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[0].y = 105;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[1].x = 1058;
            abdparam->IdaCfgSet.bsdConfig.CalibInfo[i].lowDangerLine[1].y = 701;
        }
    }
    for (int i = 0; i < 4; i++)
    {
        abdparam->IdaCfgSet.bsdConfig.AlarmParam[i].alarmSensiLevel = 1;
        abdparam->IdaCfgSet.bsdConfig.AlarmParam[i].interval = 5;
        abdparam->IdaCfgSet.bsdConfig.AlarmParam[i].speedThreshold = 30;
    }

    strcpy(abdparam->IdaCfgSet.deviceinfo.model, "C807RK111");
    abdparam->IdaCfgSet.deviceinfo.signalStrength = 100;
}

int LoadDefaultLocalParam()
{
    FILE *fp = NULL;
    int ret;
    AllConfigDef_t AllConfigDef; // 系统全局配置

    AllConfigDef.configversion.config_version = 0x00000001;
    AllConfigDef.configversion.config_magic = 0x00796796;

    LoadDefaultRecordCtrol(&AllConfigDef.recordctrol);
    LoadDefaultAlarmCtrol(&AllConfigDef.alarmctrol);
    LoadDefaultAdvancedSet(&AllConfigDef.advanceset);
    LoadDefaultCommonSet(&AllConfigDef.commonset);
    LoadDefaultEncoderSet(&AllConfigDef.encoderset);
    LoadDefaultRecordSet(&AllConfigDef.recordset);
    LoadDefaultPTZSet(&AllConfigDef.ptz_set);
    LoadDefaultNetworkParam(&AllConfigDef.networkparm);
    LoadDefaultAlarmSet(&AllConfigDef.alarmset);
    LoadDefaultVideoDetectSet(&AllConfigDef.videodetectset);
    LoadDefaultLocalDisplay(&AllConfigDef.localdisplayset);
    LoadDefaultMSServerConfig(&AllConfigDef.msserverconfig);

    LoadDefaultAutoMangeParam(&AllConfigDef.automangeset);
    LoadDefaultCameralSetting(&AllConfigDef.cameraset);
    LoadDefaultBacklineStruct(&AllConfigDef.backlinepointdata);
    LoadDefaultABDParam(&AllConfigDef.abdparam);

    

    fp = fopen("/mnt/img/.hqtds", "w+");
    if (fp != NULL)
    {
        ret = fwrite(&AllConfigDef, 1, sizeof(AllConfigDef), fp);
        int drivlist_count = AllConfigDef.abdparam.IdaCfgSet.drivlist.count;
    if (drivlist_count > 0 && AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers != NULL)
    {
        fwrite(AllConfigDef.abdparam.IdaCfgSet.drivlist.drivers, sizeof(DriverInfo), drivlist_count, fp);
    }
        fflush(fp);
        fclose(fp);
        if (ret == sizeof(AllConfigDef))
        {
            DVR_DEBUG("load default param success !");
            return 0;
        }
    }
    DVR_DEBUG("load default param failed, ret=%d !", ret);

    return -1;
}

// void SetDynamicRuntimeParams()
// {
//     allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.basicStatus_t.cpuUsage = 11;
//     allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.basicStatus_t.memoryUsage = 12;
//     allConfig.AllConfigDef.abdparam.IdaCfgSet.runstatus.basicStatus_t.temperature = 13;
// }

/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
int ReadLocalParam(void)
{
    FILE *fp = NULL;
    int ret;
    DVR_U8_T i;

    pthread_mutex_init(&allConfig.config_lock, NULL);
    allConfig.init_mutexLock = 1;
    fp = fopen("/mnt/img/.hqtds", "r");
    if (fp != NULL)
    {
        DVR_DEBUG("fp != NULL\n");
        DVR_DEBUG("sizeof(AllConfigDef_t) = %lu", sizeof(AllConfigDef_t));  
        ret = fread(&allConfig.AllConfigDef, sizeof(AllConfigDef_t), 1, fp);
        for(int t = 0; t < 4; t++){
        DVR_DEBUG("allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.activeChannel[%d] = %d\n", t, allConfig.AllConfigDef.abdparam.IdaCfgSet.devicecfg.ahdConfig.channelEnabled[t]);
        }
        DVR_DEBUG("fread returned: %d", ret);
                DVR_DEBUG("kitID111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.kitID);
        DVR_DEBUG("email111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.email);
        DVR_DEBUG("phone111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.phoneNumber);
        DVR_DEBUG("licensePlate111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.licensePlate);
        DVR_DEBUG("port111 = %d\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.port);
        DVR_DEBUG("vinNumber111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.vinNumber);
        DVR_DEBUG("imei111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.imei);
        DVR_DEBUG("model111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.model);
        DVR_DEBUG("serialNumber111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.serialNumber);
        DVR_DEBUG("mac111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.mac);
        DVR_DEBUG("ip111 = %s\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.ip);
        DVR_DEBUG("valid111 = %d\n", allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.valid);
        for (int i = 0; i < allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.deviceCount; ++i) {
                DVR_DEBUG("name[%d] = %s\n", i, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].name);
                for(int j = 0; j < allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cameraCount; ++j){
                    DVR_DEBUG("id[%d] = %d\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].id);
                    DVR_DEBUG("label[%d] = %s\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].label);
                    DVR_DEBUG("protocolType[%d] = %d\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].protocolType);
                    DVR_DEBUG("endpoint[%d] = %s\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].endpoint);
                    DVR_DEBUG("port[%d] = %d\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].port);
                    DVR_DEBUG("username[%d] = %s\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].username);
                    DVR_DEBUG("password[%d] = %s\n", j, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].cams[j].password);
                }
                for(int k = 0; k < allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttonCount; ++k){
                    DVR_DEBUG("pinNumber[%d] = %d\n", k, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttons[k].pinNumber);
                    DVR_DEBUG("type[%d] = %d\n", k, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttons[k].type);
                    DVR_DEBUG("serialNumber[%d] = %s\n", k, allConfig.AllConfigDef.abdparam.IdaCfgSet.createkitresponsedata.devices[i].buttons[k].serialNumber);
                }
        }
        if (ret != 1)
        {
            DVR_DEBUG("read local param fail,load default parameter!!!");
            fclose(fp);
            return LoadDefaultLocalParam();
        }
    }
    else
    {
        DVR_DEBUG("local parameter missing, load default parameter!!!");
        try_set_timezone_and_update("GMT+0:00", NULL);
        int dst_flag = load_last_dst_status();
        if (dst_flag == 1)
        {
            DVR_DEBUG("[DST-RECOVER-BOOT] 检测到上次处于夏令时，直接回退一小时！");

            // 1. 获取当前本地时间
            time_t t = time(NULL);
            struct tm now_tm;
            localtime_r(&t, &now_tm);

            // 2. 减去1小时
            now_tm.tm_hour -= 1;
            mktime(&now_tm); // 标准化结构体，避免跨天bug

            // 3. 写回RTC（带录像暂停恢复）
            CommonSet_t cs = {0};
            cs.system_date.year = now_tm.tm_year + 1900;
            cs.system_date.month = now_tm.tm_mon + 1;
            cs.system_date.day = now_tm.tm_mday;
            cs.system_date.hour = now_tm.tm_hour;
            cs.system_date.minute = now_tm.tm_min;
            cs.system_date.second = now_tm.tm_sec;
            cs.system_date.millisecond = 0;

            PauseAllRecording();
            Hwclock_RtcSetTime(&cs);
            ResumeAllRecording(&g_deviceconfig);

            DVR_DEBUG("[DST-RECOVER-BOOT] 已手动回退1小时，RTC写入: %04d-%02d-%02d %02d:%02d:%02d",
                      cs.system_date.year, cs.system_date.month, cs.system_date.day,
                      cs.system_date.hour, cs.system_date.minute, cs.system_date.second);
        }

        save_last_dst_status(0); // 重置dst标志
        return LoadDefaultLocalParam();
    }
    for (i = 0; i < DOORDVR_CHANNEL_NUM; i++)
    {
        DVR_DEBUG("channel[%d], name %s", i, allConfig.AllConfigDef.localdisplayset.channel_name[i]);
    }
    fclose(fp);
    ensure_timezone_anchor_file();
    return 0;
}
/***********************************************************************************************************
**函数:
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
void SystemGlobal_Get_Config_Param(void)
{
#if 1
    DVR_I32_T ret;
    void *receive_data;
    // DVR_DEBUG("11111111111111111111111111111AAllConfigDef_t(%d)",sizeof(AllConfigDef_t));
    MessageQueue_Create_Process();
    //		DVR_DEBUG("11111111111111111111111111111B");
    /*录像控制*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_RecordCtr_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(RecodCtrol_t))
        {
            memcpy(&allConfig.AllConfigDef.recordctrol, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
            // printf("recordctro = %d \r\n",allConfig.AllConfigDef.recordctrol.recordctrol[7]);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_RecordCtr_Parma_Set_ACK, NULL, 0, MSP_CMD_RecordCtr_Parma_Set_ACK);
    // DVR_DEBUG("11111111111111111111111111111CAlarmCtrol_t(%d)",sizeof(AlarmCtrol_t));
    /*报警控制*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_AlarmCtr_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(AlarmCtrol_t))
        {
            memcpy(&allConfig.AllConfigDef.alarmctrol, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_AlarmCtr_Parma_Set_ACK, NULL, 0, MSP_CMD_AlarmCtr_Parma_Set_ACK);
    // DVR_DEBUG("11111111111111111111111111111DAdvancedSet_t(%d)",sizeof(AdvancedSet_t));
    /*高级选项*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Advanced_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(AdvancedSet_t))
        {
            memcpy(&allConfig.AllConfigDef.advanceset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error DataLength=%d,%d", ((MspSendCmd_t *)receive_data)->cmdDataLength, sizeof(AdvancedSet_t));
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }

    MessageQueue_Send_Process(MSP_CMD_Advanced_Parma_Set_ACK, NULL, 0, MSP_CMD_Advanced_Parma_Set_ACK);
    // DVR_DEBUG("11111111111111111111111111111D");

    /*常规设置*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Common_FROM_UI);
    receive_data = MessageQueue_Receive_Data();

    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(CommonSet_t))
        {
            memcpy(&allConfig.AllConfigDef.commonset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_Common_Parma_Set_ACK, NULL, 0, MSP_CMD_Common_Parma_Set_ACK);
    /*编码设置*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Encoder_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(EncoderSet_t))
        {
            memcpy(&allConfig.AllConfigDef.encoderset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }

    printf("\nmirror: ");
    for (int i = 0; i < 6; i++)
        printf("%d ", allConfig.AllConfigDef.encoderset.recorde_param[i].mirror);
    printf("\nflip: ");
    for (int j = 0; j < 6; j++)
        printf("%d ", allConfig.AllConfigDef.encoderset.recorde_param[j].flip);
    printf("\n");
    /*
        DVR_DEBUG("ch=0 encoder_size=%d bit_con=%d bit_qua=%d bit_size=%d",allConfig.AllConfigDef.encoderset.recorde_param[0].encoder_size,\
            allConfig.AllConfigDef.encoderset.recorde_param[0].bitstreame_control,\
            allConfig.AllConfigDef.encoderset.recorde_param[0].bitstreame_quality,\
            allConfig.AllConfigDef.encoderset.recorde_param[0].bitstreame_size);
        DVR_DEBUG("ch=1 encoder_size=%d bit_con=%d bit_qua=%d bit_size=%d",allConfig.AllConfigDef.encoderset.recorde_param[1].encoder_size,\
            allConfig.AllConfigDef.encoderset.recorde_param[1].bitstreame_control,\
            allConfig.AllConfigDef.encoderset.recorde_param[1].bitstreame_quality,\
            allConfig.AllConfigDef.encoderset.recorde_param[1].bitstreame_size);
    */
    MessageQueue_Send_Process(MSP_CMD_Encoder_Parma_Se_ACK, NULL, 0, MSP_CMD_Encoder_Parma_Se_ACK);
    /*录像设置*/
    // DVR_DEBUG("11111111111111111111111111111E");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Recorder_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(RecordSet_t))
        {
            memcpy(&allConfig.AllConfigDef.recordset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_Recorder_Parma_Set_ACK, NULL, 0, MSP_CMD_Recorder_Parma_Set_ACK);
    /*云台设置*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Ptz_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(PTZSet_t))
        {

            memcpy(&allConfig.AllConfigDef.ptz_set, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_Ptz_Parma_Set_ACK, NULL, 0, MSP_CMD_Ptz_Parma_Set_ACK);
    //	DVR_DEBUG("111111Message_Network_Parma_Set_ACK(%d)",Message_Network_Parma_Set_ACK);
    /*网络设置*/
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Network_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(NetworkParam_t))
        {
            memcpy(&allConfig.AllConfigDef.networkparm, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(Message_Network_Parma_Set_ACK, NULL, 0, Message_Network_Parma_Set_ACK);
    /*报警设置*/
    // DVR_DEBUG("11111111111111111111111111111FAlarmSet_t(%d),NetworkParam_t(%d)",sizeof(AlarmSet_t),sizeof(NetworkParam_t));
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Alarm_Parma_FROM_UI);
    //	DVR_DEBUG("111111");
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(AlarmSet_t))
        {
            memcpy(&allConfig.AllConfigDef.alarmset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }
    //		DVR_DEBUG("111111222222");
    MessageQueue_Send_Process(MSP_CMD_Alarm_Parma_Set_ACK, NULL, 0, MSP_CMD_Alarm_Parma_Set_ACK);
    /*视频检测*/
    //	DVR_DEBUG("11111111111111111111111111111K");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Videodetect_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(VideoDetectSet_t))
        {
            memcpy(&allConfig.AllConfigDef.videodetectset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_RecordCtr_FROM_UI cmd is error ");
    }

    MessageQueue_Send_Process(MSP_CMD_Videodetect_Parma_Set_ACK, NULL, 0, MSP_CMD_Videodetect_Parma_Set_ACK);
    /*视频丢失和侦测区域*/
    //	DVR_DEBUG("11111111111111111111111111111J");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Videolost_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(VideoDetectSet_t) - sizeof(Detect_t) * 8)
        {
            memcpy(&allConfig.AllConfigDef.videodetectset.videolost_detect, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_Videolost_Parma_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_Videolost_Parma_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_VideoLost_Parma_Set_ACK, NULL, 0, MSP_CMD_VideoLost_Parma_Set_ACK);
    /*本地显示*/
    // DVR_DEBUG("11111111111111111111111111111H");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_Localdisplay_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(LocalDisplay_T))
        {
            memcpy(&allConfig.AllConfigDef.localdisplayset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), sizeof(LocalDisplay_T));
            DVR_DEBUG("receive LocalDisplay_T name:%s,%s,%s,%s", allConfig.AllConfigDef.localdisplayset.channel_name[0],
                      allConfig.AllConfigDef.localdisplayset.channel_name[1], allConfig.AllConfigDef.localdisplayset.channel_name[2],
                      allConfig.AllConfigDef.localdisplayset.channel_name[3]);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_Localdisplay_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_Localdisplay_FROM_UI cmd is error ");
    }
    // DVR_DEBUG("channel 0 NAME=%s",allConfig.AllConfigDef.localdisplayset.channel_name[0]);
    // DVR_DEBUG("channel 1 NAME=%s",allConfig.AllConfigDef.localdisplayset.channel_name[1]);
    MessageQueue_Send_Process(LOCAL_DISPLAY_ACK, NULL, 0, LOCAL_DISPLAY_ACK);
    /*服务器设置*/
    //	DVR_DEBUG("11111111111111111111111111111I %d",LOCAL_DISPLAY_ACK);
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_ServerConfig_Parma_FROM_UI);
    //	DVR_DEBUG("11111111111111111111111111111I1");
    receive_data = MessageQueue_Receive_Data();
    // DVR_DEBUG("11111111111111111111111111111I2");
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(MSServerConfig_t))
        {
            memcpy(&allConfig.AllConfigDef.msserverconfig, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_ServerConfig_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_ServerConfig_FROM_UI cmd is error ");
    }
    //	DVR_DEBUG("11111111111111111111111111111I3");
    MessageQueue_Send_Process(MSP_CMD_ServerConfig_Parma_Set_ACK, NULL, 0, MSP_CMD_ServerConfig_Parma_Set_ACK);
    /*车辆管理设置*/
    // DVR_DEBUG("11111111111111111111111111111G");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_AutoMange_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(AutoMangeParam_t))
        {
            memcpy(&allConfig.AllConfigDef.automangeset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_AutoMange_Parma_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_AutoMange_Parma_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_AutoMange_Parma_Set_ACK, NULL, 0, MSP_CMD_AutoMange_Parma_Set_ACK);
    /*摄像头触发设置*/

    // DVR_DEBUG("11111111111111111111111111111");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_CameraSetting_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(CameralSetting_t))
        {
            memcpy(&allConfig.AllConfigDef.cameraset, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_CameraSetting_Parma_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_CameraSetting_Parma_FROM_UI cmd is error ");
    }
    MessageQueue_Send_Process(MSP_CMD_CameraSetting_Parma_Set_ACK, NULL, 0, MSP_CMD_CameraSetting_Parma_Set_ACK);
    /*ABD算法参数设置*/

    // DVR_DEBUG("11111111111111111111111111111");
    ret = MessageQueue_Receive_Process(MESSAGE_TYPE_ABDSetting_Parma_FROM_UI);
    receive_data = MessageQueue_Receive_Data();
    if (ret > 0)
    {
        if (((MspSendCmd_t *)receive_data)->cmdDataLength == sizeof(ABDParam_t))
        {
            memcpy(&allConfig.AllConfigDef.abdparam, (DVR_U8_T *)(((MspSendCmd_t *)receive_data)->cmdData), ((MspSendCmd_t *)receive_data)->cmdDataLength);
        }
        else
        {
            DVR_ERROR("receive message MESSAGE_TYPE_CameraSetting_Parma_FROM_UI length is error ");
        }
    }
    else
    {
        DVR_ERROR("receive message MESSAGE_TYPE_CameraSetting_Parma_FROM_UI cmd is error ");
    }
    DVR_DEBUG("<<<<<<<< allConfig.AllConfigDef.abdparam.AlgorithmSelect.DMS_Enable=%d,ADAS_Enable=%d, \
        BSD_Enable=%d,BSDCount=%d,Available_BSDChn_num=%d,[dms adas bsd1 bsd2 bsd3 bsd4]->[%d %d %d %d %d %d]\n",
              allConfig.AllConfigDef.abdparam.AlgorithmSelect.DMS_Enable,
              allConfig.AllConfigDef.abdparam.AlgorithmSelect.ADAS_Enable, allConfig.AllConfigDef.abdparam.AlgorithmSelect.BSD_Enable,
              allConfig.AllConfigDef.abdparam.IdaCfgSet.bsdConfig.BSDCount, allConfig.AllConfigDef.abdparam.algVideoChnSelect.Available_BSDChn_num,
              allConfig.AllConfigDef.abdparam.algVideoChnSelect.DMS_VideoChn_num, allConfig.AllConfigDef.abdparam.algVideoChnSelect.ADAS_VideoChn_num,
              allConfig.AllConfigDef.abdparam.algVideoChnSelect.BSD1_VideoChn_num, allConfig.AllConfigDef.abdparam.algVideoChnSelect.BSD2_VideoChn_num,
              allConfig.AllConfigDef.abdparam.algVideoChnSelect.BSD3_VideoChn_num, allConfig.AllConfigDef.abdparam.algVideoChnSelect.BSD4_VideoChn_num);

    DVR_DEBUG("allConfig.AllConfigDef.abdparam.adasAditonCtrl.open_close_laneline_mark=%d,open_close_yuv_capture=%d",
              allConfig.AllConfigDef.abdparam.adasAditonCtrl.open_close_laneline_mark, allConfig.AllConfigDef.abdparam.adasAditonCtrl.open_close_yuv_capture);
    MessageQueue_Send_Process(MSP_CMD_ABDSetting_Parma_Set_ACK, NULL, 0, MSP_CMD_ABDSetting_Parma_Set_ACK);

#endif

#if 0
	allConfig.AllConfigDef.encoderset.recorde_param[0].encoder_size = 6;
	allConfig.AllConfigDef.encoderset.recorde_param[1].encoder_size = 6;
	allConfig.AllConfigDef.encoderset.recorde_param[4].encoder_size = 6;
	allConfig.AllConfigDef.encoderset.recorde_param[5].encoder_size = 6;
#endif
}

/***********************************************************************************************************
**函数: SystemGlobal_GetAllconfigDefContext
**输入参数:
**功能:
**返回值:
***********************************************************************************************************/
AllConfigDef_t *SystemGlobal_GetAllconfigDefContext(void)
{
    time_t timep;
    struct tm *p;
    time(&timep);
    p = localtime(&timep);

    allConfig.AllConfigDef.commonset.system_date.year = 1900 + p->tm_year;
    allConfig.AllConfigDef.commonset.system_date.month = 1 + p->tm_mon;
    allConfig.AllConfigDef.commonset.system_date.day = p->tm_mday;
    allConfig.AllConfigDef.commonset.system_date.hour = p->tm_hour;
    allConfig.AllConfigDef.commonset.system_date.minute = p->tm_min;
    allConfig.AllConfigDef.commonset.system_date.second = p->tm_sec;

    return &allConfig.AllConfigDef;
}

int SystemGlobal_SetAllconfigDefAdasCH(uint8_t chn)
{
    allConfig.AllConfigDef.abdparam.algVideoChnSelect.ADAS_VideoChn_num = chn;
    DVR_DEBUG("allConfig.AllConfigDef.abdparam.algVideoChnSelect.ADAS_VideoChn_num=%d", allConfig.AllConfigDef.abdparam.algVideoChnSelect.ADAS_VideoChn_num);
    return 0;
}


int DVR_GetIdaPara(ABDParam_t *ABDParam, ABDParam_t *paramABD)
{
    DVR_U32_T nRetSize = -1;

    if (ABDParam && paramABD)
    {
        // DVR_DEBUG("paramABD->AlgorithmSelect-[%d,%d,%d],BSDCount=%d,Available_BSDChn_num=%d,CHN[DMS,ADAS,BSD1,BSD2,BSD3,BSD4]->[%d,%d,%d,%d,%d,%d]\n",
        // paramABD->AlgorithmSelect.DMS_Enable, paramABD->AlgorithmSelect.ADAS_Enable,
        // paramABD->AlgorithmSelect.BSD_Enable, paramABD->IdaCfgSet.bsdConfig.BSDCount,paramABD->algVideoChnSelect.Available_BSDChn_num,
        // paramABD->algVideoChnSelect.DMS_VideoChn_num,paramABD->algVideoChnSelect.ADAS_VideoChn_num,
        // paramABD->algVideoChnSelect.BSD1_VideoChn_num,paramABD->algVideoChnSelect.BSD2_VideoChn_num,
        // paramABD->algVideoChnSelect.BSD3_VideoChn_num,paramABD->algVideoChnSelect.BSD4_VideoChn_num);

        nRetSize = sizeof(ABDParam_t);
        memcpy(ABDParam, paramABD, nRetSize);

        // DVR_DEBUG("ABDParam->AlgorithmSelect-[%d,%d,%d],BSDCount=%d,Available_BSDChn_num=%d,CHN[DMS,ADAS,BSD1,BSD2,BSD3,BSD4]->[%d,%d,%d,%d,%d,%d]\n",
        // ABDParam->AlgorithmSelect.DMS_Enable, ABDParam->AlgorithmSelect.ADAS_Enable,
        // ABDParam->AlgorithmSelect.BSD_Enable,  ABDParam->IdaCfgSet.bsdConfig.BSDCount,paramABD->algVideoChnSelect.Available_BSDChn_num,
        // ABDParam->algVideoChnSelect.DMS_VideoChn_num,ABDParam->algVideoChnSelect.ADAS_VideoChn_num,
        // ABDParam->algVideoChnSelect.BSD1_VideoChn_num,ABDParam->algVideoChnSelect.BSD2_VideoChn_num,
        // ABDParam->algVideoChnSelect.BSD3_VideoChn_num,ABDParam->algVideoChnSelect.BSD4_VideoChn_num);
    }
    // DVR_DEBUG("nRetSize=%d",nRetSize);
    return nRetSize;
}

int DVR_SetIdaPara(ABDParam_t *ABDParam, ABDParam_t *paramABD)
{
    DVR_U32_T nRetSize = -1;

    if (ABDParam && paramABD)
    {
        // DVR_DEBUG("paramABD->AlgorithmSelect-[%d,%d,%d],BSDCount=%d,Available_BSDChn_num=%d,CHN[DMS,ADAS,BSD1,BSD2,BSD3,BSD4]->[%d,%d,%d,%d,%d,%d]\n",
        // paramABD->AlgorithmSelect.DMS_Enable, paramABD->AlgorithmSelect.ADAS_Enable,
        // paramABD->AlgorithmSelect.BSD_Enable, paramABD->IdaCfgSet.bsdConfig.BSDCount,paramABD->algVideoChnSelect.Available_BSDChn_num,
        // paramABD->algVideoChnSelect.DMS_VideoChn_num,paramABD->algVideoChnSelect.ADAS_VideoChn_num,
        // paramABD->algVideoChnSelect.BSD1_VideoChn_num,paramABD->algVideoChnSelect.BSD2_VideoChn_num,
        // paramABD->algVideoChnSelect.BSD3_VideoChn_num,paramABD->algVideoChnSelect.BSD4_VideoChn_num);

        nRetSize = sizeof(ABDParam_t);
        memcpy(paramABD, ABDParam, nRetSize);

        // DVR_DEBUG("ABDParam->AlgorithmSelect-[%d,%d,%d],BSDCount=%d,Available_BSDChn_num=%d,CHN[DMS,ADAS,BSD1,BSD2,BSD3,BSD4]->[%d,%d,%d,%d,%d,%d]\n",
        // ABDParam->AlgorithmSelect.DMS_Enable, ABDParam->AlgorithmSelect.ADAS_Enable,
        // ABDParam->AlgorithmSelect.BSD_Enable,  ABDParam->IdaCfgSet.bsdConfig.BSDCount,paramABD->algVideoChnSelect.Available_BSDChn_num,
        // ABDParam->algVideoChnSelect.DMS_VideoChn_num,ABDParam->algVideoChnSelect.ADAS_VideoChn_num,
        // ABDParam->algVideoChnSelect.BSD1_VideoChn_num,ABDParam->algVideoChnSelect.BSD2_VideoChn_num,
        // ABDParam->algVideoChnSelect.BSD3_VideoChn_num,ABDParam->algVideoChnSelect.BSD4_VideoChn_num);
    }
    // DVR_DEBUG("nRetSize=%d",nRetSize);
    return nRetSize;
}

// #ifdef __cplusplus
// #if __cplusplus
// }
// #endif /* __cplusplus */
// #endif  /* __cplusplus */
