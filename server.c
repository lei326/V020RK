#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "socket.h"
#define _GNU_SOURCE
#include <sys/prctl.h>
#include "server.h"
#include "server_param.h"
#include "ms_netdvr_playback.h"
#include "rtmp_playback.h"
#include "cJSON.h"
#include "rtmp_playback.h"  
#include "rtmp_ms.h"  
#include <dirent.h>
FILE *g_server_log_fp = NULL;
#define MAX_LOG_SIZE   (1*1024*1024)    
#define MAX_LOG_FILES  10  
int current_log_index = 1;
#define MAX_LOG_INDEX   50       
time_t parseTime(const char* timeStr)
{
    struct tm tm_time;
    memset(&tm_time, 0, sizeof(tm_time));
    strptime(timeStr, "%Y%m%d-%H%M%S", &tm_time);
    return mktime(&tm_time);
}

void make_log_dir() {
    struct stat st = {0};
    if (stat("/userdata/log/", &st) == -1) {
        if (mkdir("/userdata/log/", 0777) != 0) {
            perror("mkdir /userdata/log failed");
            exit(EXIT_FAILURE);
        }
        chmod("/userdata/log/", 0777);
    }
}

void get_next_log_index() {
    int max_idx = 0;
    DIR *dir = opendir("/userdata/log/");
    struct dirent *entry;
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            int idx = 0;
            if (sscanf(entry->d_name, "server%d.log", &idx) == 1) {
                if (idx > max_idx) max_idx = idx;
            }
        }
        closedir(dir);
    }
    if (max_idx == 0)
        max_idx = 1;
    else {
        max_idx = max_idx + 1;
        if (max_idx > MAX_LOG_INDEX)
            max_idx = 1;
    }
    current_log_index = max_idx;
}

void open_current_log_file() {
    char filename[256];
    snprintf(filename, sizeof(filename), "/userdata/log/server%d.log", current_log_index);
    g_server_log_fp = fopen(filename, "a+");
    if (!g_server_log_fp) {
        perror("open log file failed");
        exit(EXIT_FAILURE);
    }
    setbuf(g_server_log_fp, NULL);
}

void delete_oldest_log_file() {
    int oldest_index = current_log_index - MAX_LOG_FILES;
    if (oldest_index <= 0)
        oldest_index += MAX_LOG_INDEX;
    char path[256];
    snprintf(path, sizeof(path), "/userdata/log/server%d.log", oldest_index);
    remove(path);
}

void check_and_advance_log() {
    if (!g_server_log_fp) return;
    fflush(g_server_log_fp);
    char filename[256];
    snprintf(filename, sizeof(filename), "/userdata/log/server%d.log", current_log_index);
    struct stat st;
    if (stat(filename, &st) == 0 && st.st_size >= MAX_LOG_SIZE) {
        fclose(g_server_log_fp);
        g_server_log_fp = NULL;

        current_log_index++;
        if (current_log_index > MAX_LOG_INDEX)
            current_log_index = 1;

        delete_oldest_log_file();

        open_current_log_file();
    }
}

void server_log_init() {
    make_log_dir();
    get_next_log_index();
    open_current_log_file();
}

#define server_printf(fmt, ...) \
    do { \
        check_and_advance_log(); \
        if (g_server_log_fp) { \
            fprintf(g_server_log_fp, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#ifdef __cplusplus
extern "C"
{
#endif
    extern int rk_get_incident_images(const IncidentImageRequest_t *request, IncidentImageResponse_t *response);
    extern int rk_create_kit(const CreateKitRequest_t *request, CreateKitResponseData_t *response);
    extern int rk_get_device_info(const DeviceInfoRequest_t *request, DeviceInfoResponse_t *response);
    extern int rk_get_current_location(const CurrentLocationRequest_t *request, CurrentLocationResponse_t *response);
    extern int rk_get_health_check(const HealthCheckRequest_t *request, HealthCheckResponse_t *response);
    extern int rk_config_device_settings(const ConfigDeviceSettingsRequest_t *request, ConfigDeviceSettingsResponse_t *response);
    extern int rk_create_incident(const CreateIncidentRequest_t *request, CreateIncidentResponse_t *response);
    extern int rk_capture_clear_records();
    extern int rk_clear_driver_register_info();
    extern int rk_capture_get_records(CaptureRecordList_t *recordlist);
    extern int rk_get_debug_feature_config(DebugFeatureConfig_t *cfg);
    extern int rk_set_debug_feature_config(DebugFeatureConfig_t *cfg);
    extern int rk_capture_set_config(CaptureConfig_t *captureconfig);
    extern int rk_capture_get_config(CaptureConfig_t *captureconfig);
    extern int rk_driver_register(DriverInfo_t *drive_info);
	extern int rk_adas_get_preview_config(ADASPreviewConfig_t *config);
	extern int rk_adas_set_preview_config(ADASPreviewConfig_t *config);
	extern int rk_dms_get_config(DMSConfig_t *cfg);
	extern int rk_dms_set_config(DMSConfig_t *cfg);
	extern int rk_adas_get_config(ADASConfig_t *cfg);
	extern int rk_adas_set_config(ADASConfig_t *cfg);
	extern int ms_record_play_control(const PlayControl_t* control);
	extern int rk_get_record_file_list(RecordQueryParams_t* params, char* recordList);
	extern int rk_storage_get_stream_config(RecordStreamConfig_t* config);
	extern int rk_storage_set_stream_config(const RecordStreamConfig_t* config);
	extern int rk_storage_get_config(StorageConfig_t* config);
	extern int rk_storage_set_config(const StorageConfig_t* config);
	extern int rk_storage_format();
	extern int rk_storage_get_disk_format_status();
	//extern int rk_get_device_info(DeviceInfo_t* deviceif);
	extern int rk_get_running_status(RunningStatus_t* RunStatus);
	extern int rk_get_product_info(ProductInfo_t* proinfo);
    extern int rk_get_device_config(DeviceConfig_t* deviceconfig);
    extern int rk_set_device_config(DeviceConfig_t* deviceconfig);
    extern int rk_format_tf_card();
    extern int rk_driver_delete_info();
    extern int rk_driver_get_list(DriverList_t *driverlist);
    extern int rk_verify_get_records(VerifyRecordList_t *verifyrecordlist);
    extern int rk_verify_clear_records();
    extern int rk_record_file_play(PlayBack_t* params, MS_LOCAL_REC_INFO* info);
	extern int rk_system_upgrade(const char *path);
    extern int rk_system_get_update_progress(int *value);
    extern int rk_system_get_update_type(int *value);
	static int listen_fd = 0;
	static pthread_t RkIpcServerTid = 0;
	static int RkIpcServerRun = 0;

	struct FunMap
	{
		char *fun_name;
		int (*fun)(int);
	};

int ser_rk_create_kit(int fd)
{
    server_printf("[%s] Start!! fd = %d\n", __func__, fd);

    CreateKitRequest_t req;
    CreateKitResponseData_t resp;
    int ret = 0;

    memset(&resp, 0, sizeof(resp));

    if (sock_read(fd, &req, sizeof(req)) == SOCKERR_CLOSED) {
        server_printf("[%s] Failed to read CreateKit request\n", __func__);
        return -1;
    }

    ret = rk_create_kit(&req, &resp);


    if (sock_write(fd, &resp, sizeof(resp)) == SOCKERR_CLOSED) {
        server_printf("[%s] Failed to write response data\n", __func__);
        return -1;
    }
    printf("vaild = %d\n", resp.valid);
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] Failed to write return value\n", __func__);
        return -1;
    }

    return 0;
}

int ser_rk_get_incident_images(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);

    IncidentImageRequest_t  req;
    IncidentImageResponse_t resp;
    int ret = 0;

    // 读取请求
    if (sock_read(fd, &req, sizeof(req)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_read request failed!\n", __func__);
        return -1;
    }
    server_printf("[%s] Received request from client: eventID=%s, kitID=%s\n", __func__, req.eventID, req.kitID);

    memset(&resp, 0, sizeof(resp));
    ret = rk_get_incident_images(&req, &resp);
    if (ret != 0) {
        server_printf("[%s] rk_get_incident_images failed, ret=%d\n", __func__, ret);
        return -1;
    }
    server_printf("[%s] Retrieved incident images successfully, imageCount=%d\n", __func__, resp.imageCount);

    // 写入 imageCount
    if (sock_write(fd, &resp.imageCount, sizeof(resp.imageCount)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write imageCount failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent imageCount=%d to client\n", __func__, resp.imageCount);

    // 写入 kitID
    if (sock_write(fd, resp.kitID, sizeof(resp.kitID)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write kitID failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent kitID=%s to client\n", __func__, resp.kitID);

    // 写入 eventID
    if (sock_write(fd, resp.eventID, sizeof(resp.eventID)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write eventID failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent eventID=%s to client\n", __func__, resp.eventID);

    // 写入 isValid
    if (sock_write(fd, &resp.isValid, sizeof(resp.isValid)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write isValid failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent isValid=%d to client\n", __func__, resp.isValid);

    // 写入 error 信息
    int errLen = 0;
    if (resp.error) errLen = (int)strlen(resp.error);
    if (sock_write(fd, &errLen, sizeof(errLen)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write error length failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent error length=%d to client\n", __func__, errLen);
    if (errLen > 0) {
        if (sock_write(fd, resp.error, (size_t)errLen) == SOCKERR_CLOSED) {
            server_printf("[%s] sock_write error bytes failed!\n", __func__);
            goto out_err;
        }
        server_printf("[%s] Sent error message to client: %s\n", __func__, resp.error);
    }

    // 写入图片数据
    if (resp.imageCount > 0 && resp.images) {
        server_printf("[%s] Sending %d images to client\n", __func__, resp.imageCount);
        for (int i = 0; i < resp.imageCount; ++i) {
            if (sock_write(fd, &resp.images[i].id, sizeof(resp.images[i].id)) == SOCKERR_CLOSED) {
                server_printf("[%s] sock_write image[%d].id failed!\n", __func__, i);
                goto out_err;
            }
            server_printf("[%s] Sent image[%d].id=%d to client\n", __func__, i, resp.images[i].id);

            int fileLen = resp.images[i].fileSize;
            if (fileLen < 0) fileLen = 0;
            if (resp.images[i].file == NULL) fileLen = 0;

            if (sock_write(fd, &fileLen, sizeof(fileLen)) == SOCKERR_CLOSED) {
                server_printf("[%s] sock_write image[%d].fileSize failed!\n", __func__, i);
                goto out_err;
            }
            server_printf("[%s] Sent image[%d].fileSize=%d to client\n", __func__, i, fileLen);

            if (fileLen > 0) {
                if (sock_write(fd, resp.images[i].file, (size_t)fileLen) == SOCKERR_CLOSED) {
                    server_printf("[%s] sock_write image[%d].file bytes failed!\n", __func__, i);
                    goto out_err;
                }
                server_printf("[%s] Sent image[%d].file data to client\n", __func__, i);
            }
        }
    }

    // 写入返回值
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write ret failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent ret=%d to client\n", __func__, ret);

    // 资源释放
    if (resp.images) {
        for (int i = 0; i < resp.imageCount; ++i) {
            if (resp.images[i].file) {
                free(resp.images[i].file);
                resp.images[i].file = NULL;
                server_printf("[%s] Freed memory for image[%d].file\n", __func__, i);
            }
        }
        free(resp.images);
        resp.images = NULL;
        server_printf("[%s] Free memory for images array\n", __func__);
    }
    if (resp.error) {
        free(resp.error);
        resp.error = NULL;
        server_printf("[%s] Free memory for error message\n", __func__);
    }

    return 0;

out_err:
    server_printf("[%s] Error occurred during processing, freeing resources\n", __func__);
    if (resp.images) {
        for (int i = 0; i < resp.imageCount; ++i) {
            if (resp.images[i].file) {
                free(resp.images[i].file);
                resp.images[i].file = NULL;
            }
        }
        free(resp.images);
        resp.images = NULL;
    }
    if (resp.error) {
        free(resp.error);
        resp.error = NULL;
    }
    return -1;
}

int ser_rk_get_device_info(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);

    DeviceInfoRequest_t  req;
    DeviceInfoResponse_t resp;
    int ret = 0;

    // 读请求
    if (sock_read(fd, &req, sizeof(req)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_read request failed!\n", __func__);
        return -1;
    }
    server_printf("[%s] Received request: kitID=%s\n", __func__, req.kitID);

    memset(&resp, 0, sizeof(resp));
    ret = rk_get_device_info(&req, &resp);
    if (ret != 0) {
        server_printf("[%s] rk_get_device_info failed, ret=%d\n", __func__, ret);
        return -1; 
    }
    server_printf("[%s] Retrieved device info, cameraCount=%d\n",
                  __func__, resp.kitInfoDto.cameraCount);

    if (sock_write(fd, &resp, sizeof(resp)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write kitInfoDto block failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent DeviceKitInfo_t block (%zu bytes)\n",
                  __func__, sizeof(resp.kitInfoDto));

    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write ret failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent ret=%d\n", __func__, ret);

    return 0;

out_err:
    server_printf("[%s] Error occurred during processing\n", __func__);
    return -1;
}

int ser_rk_get_current_location(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);

    CurrentLocationRequest_t  req;
    CurrentLocationResponse_t resp;
    memset(&resp, 0, sizeof(resp));
    int ret = 0;

    if (sock_read(fd, &req, sizeof(req)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_read request failed!\n", __func__);
        return -1;
    }
    server_printf("[%s] Received request: kitID=%s\n", __func__, req.kitID);
    ret = rk_get_current_location(&req, &resp);
    if (ret != 0) {
        server_printf("[%s] rk_get_current_location failed, ret=%d\n", __func__, ret);
        return -1; 
    }

    server_printf("[%s] Location ok: kitID=%s, plate=%s, lat=%s, lon=%s, dir=%d, radius=%d\n",
                  __func__,
                  resp.locationData.kitID,
                  resp.locationData.plateNumber,
                  resp.locationData.latitude,
                  resp.locationData.longitude,
                  resp.locationData.direction,
                  resp.locationData.radius);

    if (sock_write(fd, &resp, sizeof(resp)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write response block failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent CurrentLocationResponse_t block (%zu bytes)\n",
                  __func__, sizeof(resp));

    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write ret failed!\n", __func__);
        goto out_err;
    }
    server_printf("[%s] Sent ret=%d\n", __func__, ret);

    return 0;

out_err:
    server_printf("[%s] Error occurred during processing\n", __func__);
    return -1;
}

int ser_rk_get_health_check(int fd)
{
  server_printf("[%s] called, fd=%d\n", __func__, fd);

  HealthCheckRequest_t  req;
  HealthCheckResponse_t resp;
  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));

  if (sock_read(fd, &req, sizeof(req)) == SOCKERR_CLOSED) {
    server_printf("[%s] sock_read request failed!\n", __func__);
    return -1;
  }
  server_printf("[%s] Received request: kitID=%s\n", __func__, req.kitID);

  int ret = rk_get_health_check(&req, &resp);
  if (ret != 0) {
    server_printf("[%s] rk_get_health_check failed, ret=%d\n", __func__, ret);
    return -1;
  }

  server_printf("[%s] HealthCheck OK: kitID=%s, IMEI=%s, plate=%s, cams=%d, loc=%s\n",
                __func__,
                resp.healthCheckData.kitID,
                resp.healthCheckData.kitInfoDto.IMEI,
                resp.healthCheckData.kitInfoDto.plateNumber,
                resp.healthCheckData.cameraCount,
                resp.healthCheckData.locationInfoDto.status);

  if (sock_write(fd, &resp, sizeof(resp)) == SOCKERR_CLOSED) {
    server_printf("[%s] sock_write response block failed!\n", __func__);
    goto out_err;
  }
  server_printf("[%s] Sent HealthCheckResponse_t block (%zu bytes)\n", __func__, sizeof(resp));

  if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
    server_printf("[%s] sock_write ret failed!\n", __func__);
    goto out_err;
  }
  server_printf("[%s] Sent ret=%d\n", __func__, ret);

  return 0;

out_err:
  server_printf("[%s] Error occurred during processing\n", __func__);
  return -1;
}


int ser_rk_config_device_settings(int fd)
{
  server_printf("[%s] called, fd=%d\n", __func__, fd);

  ConfigDeviceSettingsRequest_t  req;
  ConfigDeviceSettingsResponse_t resp;
  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));

  // 读请求
  if (sock_read(fd, &req, sizeof(req)) == SOCKERR_CLOSED) {
    server_printf("[%s] sock_read request failed!\n", __func__);
    return -1;
  }

  server_printf("[%s] Received request: kitID=%s, plate=%s, chasis=%s, face=%d\n",
                __func__, req.kitID, req.plateNumber, req.chasisNumber, (int)req.faceRecognition);

  int ret = rk_config_device_settings(&req, &resp);
  if (ret != 0) {
    server_printf("[%s] rk_config_device_settings failed, ret=%d\n", __func__, ret);
    return -1;
  }

  // 回写响应块
  if (sock_write(fd, &resp, sizeof(resp)) == SOCKERR_CLOSED) {
    server_printf("[%s] sock_write response block failed!\n", __func__);
    goto out_err;
  }
  server_printf("[%s] Sent ConfigDeviceSettingsResponse_t block (%zu bytes)\n", __func__, sizeof(resp));

  if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
    server_printf("[%s] sock_write ret failed!\n", __func__);
    goto out_err;
  }
  server_printf("[%s] Sent ret=%d\n", __func__, ret);

  server_printf("[%s] Mock reply: kitID=%s, plate=%s, chasis=%s, face=%d, success=%d, status=%d, msg=\"%s\"\n",
                __func__,
                resp.parsedApplied.data.kitID,
                resp.parsedApplied.data.plateNumber,
                resp.parsedApplied.data.chasisNumber,
                (int)resp.parsedApplied.data.faceRecognition,
                (int)resp.parsedApplied.success,
                resp.parsedApplied.status,
                resp.parsedApplied.message);

  return 0;

out_err:
  server_printf("[%s] Error occurred during processing\n", __func__);
  return -1;
}


int ser_rk_create_incident(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);

    CreateIncidentRequest_t req_local;
    CreateIncidentResponse_t resp_local;
    memset(&req_local, 0, sizeof(req_local));

    if (sock_read(fd, req_local.kitID, sizeof(req_local.kitID)) == SOCKERR_CLOSED) {
        server_printf("[%s] read kitID failed\n", __func__);
        return -1;
    }
    if (sock_read(fd, req_local.eventID, sizeof(req_local.eventID)) == SOCKERR_CLOSED) {
        server_printf("[%s] read eventID failed\n", __func__);
        return -1;
    }
    if (sock_read(fd, &req_local.caseStartedDateTime, sizeof(req_local.caseStartedDateTime)) == SOCKERR_CLOSED) {
        server_printf("[%s] read caseStartedDateTime failed\n", __func__);
        return -1;
    }
    if (sock_read(fd, &req_local.imageIdCount, sizeof(req_local.imageIdCount)) == SOCKERR_CLOSED) {
        server_printf("[%s] read imageIdCount failed\n", __func__);
        return -1;
    }

    int *imgids_buf = NULL;
    if (req_local.imageIdCount > 0) {
        size_t bytes = (size_t)req_local.imageIdCount * sizeof(int);
        imgids_buf = (int*)calloc((size_t)req_local.imageIdCount, sizeof(int));
        if (!imgids_buf) {
            server_printf("[%s] calloc imgids failed!\n", __func__);
            return -1;
        }
        if (sock_read(fd, imgids_buf, bytes) == SOCKERR_CLOSED) {
            server_printf("[%s] read imageIds[] failed!\n", __func__);
            free(imgids_buf);
            return -1;
        }
        req_local.incidentImageIds = imgids_buf;
    } else {
        req_local.incidentImageIds = NULL;
    }

    // 6. kitInfoDto
    if (sock_read(fd, &req_local.kitInfoDto, sizeof(req_local.kitInfoDto)) == SOCKERR_CLOSED) {
        server_printf("[%s] read kitInfoDto failed\n", __func__);
        free(imgids_buf);
        return -1;
    }

    int ret = rk_create_incident(&req_local, &resp_local);
    if (ret != 0) {
        server_printf("[%s] rk_create_incident failed, ret=%d\n", __func__, ret);
        free(imgids_buf);
        return -1;
    }

    if (sock_write(fd, &resp_local.imageIdCount, sizeof(int)) == SOCKERR_CLOSED) goto send_err;

    if (sock_write(fd, resp_local.kitID, sizeof(resp_local.kitID)) == SOCKERR_CLOSED) goto send_err;
    if (sock_write(fd, resp_local.eventID, sizeof(resp_local.eventID)) == SOCKERR_CLOSED) goto send_err;
    if (sock_write(fd, &resp_local.caseStartedDateTime, sizeof(resp_local.caseStartedDateTime)) == SOCKERR_CLOSED) goto send_err;

    if (sock_write(fd, &resp_local.isValid, sizeof(resp_local.isValid)) == SOCKERR_CLOSED) goto send_err;

    if (sock_write(fd, &resp_local.kitInfoDto, sizeof(resp_local.kitInfoDto)) == SOCKERR_CLOSED) goto send_err;
    if (resp_local.imageIdCount > 0 && resp_local.incidentImageIds) {
        size_t bytes = (size_t)resp_local.imageIdCount * sizeof(int);
        if (sock_write(fd, resp_local.incidentImageIds, bytes) == SOCKERR_CLOSED) goto send_err;
    }

    free(imgids_buf);
    free(resp_local.incidentImageIds);
    return 0;

send_err:
    server_printf("[%s] send response failed!\n", __func__);
    free(imgids_buf);
    free(resp_local.incidentImageIds);
    return -1;
}

int ser_rk_adas_get_preview_config(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    ADASPreviewConfig_t cfg;
    int ret = rk_adas_get_preview_config(&cfg);
    if (sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_write for cfg failed!\n", __func__);
        return -1;
    }
    // if (ret == 0)
    //     if (sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    //     {
    //         server_printf("[%s] sock_write (ret==0) for cfg failed!\n", __func__);
    //         return -1;
    //     }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_adas_set_preview_config(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    ADASPreviewConfig_t cfg;
    if (sock_read(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_read for cfg failed!\n", __func__);
        return -1;
    }
    int ret = rk_adas_set_preview_config(&cfg);
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int set_rk_dms_get_config(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DMSConfig_t cfg;
    int ret = rk_dms_get_config(&cfg);
    if (sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_write for cfg failed!\n", __func__);
        return -1;
    }
    if (ret == 0)
        if (sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
        {
            server_printf("[%s] sock_write (ret==0) for cfg failed!\n", __func__);
            return -1;
        }
    return 0;
}

int set_rk_dms_set_config(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DMSConfig_t cfg;
    if (sock_read(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_read for cfg failed!\n", __func__);
        return -1;
    }
    int ret = rk_dms_set_config(&cfg);
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_adas_get_config(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    ADASConfig_t cfg;
    int ret = rk_adas_get_config(&cfg);
    if (sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_write for cfg failed!\n", __func__);
        return -1;
    }
    if (ret == 0)
        if (sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
        {
            server_printf("[%s] sock_write (ret==0) for cfg failed!\n", __func__);
            return -1;
        }
    return 0;
}

int ser_rk_adas_set_config(int fd)
{
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    ADASConfig_t cfg;
    if (sock_read(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_read for cfg failed!\n", __func__);
        return -1;
    }
    int ret = rk_adas_set_config(&cfg);
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED)
    {
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}


int ser_rk_system_upgrade(int fd) {
    server_printf("[%s] called, fd=%d\n", __func__, fd);
	int ret = 0;
	int len;
	char *value = NULL;

	if (sock_read(fd, &len, sizeof(int)) == SOCKERR_CLOSED)
		return -1;
	if (len) {
		value = (char *)malloc(len);
		if (sock_read(fd, value, len) == SOCKERR_CLOSED) {
            server_printf("[%s] sock_read failed!\n", __func__);
			free(value);
			return -1;
		}
		printf("value is %s\n", value);
		ret = rk_system_upgrade(value);
		free(value);
		if (sock_write(fd, &ret, sizeof(int)) == SOCKERR_CLOSED)
        server_printf("[%s] sock_write for ret failed!\n", __func__);
			return -1;
	}

	return 0;
}


int ser_rk_system_get_update_type(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int value = 0;
    int ret = rk_system_get_update_type(&value); 
    if (ret != 0) {
        server_printf("[%s] get_update_type failed, ret=%d\n", __func__, ret);
        return -1;
    }
    if(sock_write(fd, &value, sizeof(value)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for value failed!\n", __func__);
        return -1;   
    }
    return 0;
}

int ser_rk_system_get_update_progress(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int progress = 0;
    int ret = rk_system_get_update_progress(&progress);
    if (ret != 0) {
        server_printf("[%s] get_update_progress failed, ret=%d\n", __func__, ret);
        return -1;
    }
    if (sock_write(fd, &progress, sizeof(progress)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write for progress failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_ms_record_play_control(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    printf("[%s] called, fd=%d\n", __func__, fd);
    PlayControl_t PlayControl;
    if(sock_read(fd, &PlayControl, sizeof(PlayControl)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for PlayControl failed!\n", __func__);
        return -1;
    }
    printf("收到客户端操作：operate=%d, playSpeed=%d, dragTime=%d\n",
        PlayControl.operate, PlayControl.playSpeed, PlayControl.dragTime);
    int ret = ms_record_play_control(&PlayControl);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_driver_register(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DriverInfo_t drive_info;
    if(sock_read(fd, &drive_info, sizeof(drive_info)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for PlayControl failed!\n", __func__);
        return -1;
    }
    int ret = rk_driver_register(&drive_info);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write ret failed!\n", __func__);
        return -1;
    }
}

int ser_rk_get_record_file_list(int fd)
{
    server_printf("Enter ser_rk_get_record_file_list\n");
    RecordQueryParams_t params = {0};
    int ret = 0, len = 0;

    if (sock_read(fd, &params, sizeof(params)) == SOCKERR_CLOSED) {
        printf("Failed to read query struct from client\n");
        return -1;
    }

    server_printf("RecordQueryParams_t received:\n");
    server_printf("  channel: %d\n", params.channel);
    server_printf("  recordType: %d\n", params.recordType);
    server_printf("  startTime: %ld\n", params.startTime);
    server_printf("  endTime: %ld\n", params.endTime);
    server_printf("  maxNumPerPage: %d\n", params.maxNumPerPage);
    server_printf("  currentPage: %d\n", params.currentPage);
    server_printf("  bufferLength: %d\n", params.bufferLength);

    printf("[DEBUG] Call rk_get_record_file_list with maxNumPerPage=%d, currentPage=%d\n", params.maxNumPerPage, params.currentPage);

    char *recordList = (char*)calloc(1, params.bufferLength + 2); 
    if (!recordList) {
        server_printf("Failed to allocate recordList buffer\n");
        return -1;
    }
    ret = rk_get_record_file_list(&params, recordList);

    printf("rk_get_record_file_list() 返回记录数 ret = %d\n", ret);
    printf("original recordList=[%s]\n", recordList);

    int listLen = strlen(recordList);
    if (listLen > 0 && recordList[listLen - 1] != ';') {
        recordList[listLen] = ';';
        recordList[listLen + 1] = '\0';
        printf(" recordList=[%s]\n", recordList);
    }

    cJSON* items = cJSON_CreateArray();
    int max_items = params.maxNumPerPage; 
    int count = 0;

    if (strlen(recordList) > 0) {
        char* saveptr1 = NULL;
        char* line = strtok_r(recordList, ";", &saveptr1);
        int line_index = 0;
        while (line != NULL && strlen(line) > 0) {
            printf("处理第%d条原始录像(line=%s)\n", line_index + 1, line);

            char* fields[8] = {0};
            int i = 0;
            char* saveptr2 = NULL;
            char* field = strtok_r(line, ",", &saveptr2);
            while (field != NULL && i < 8) {
                fields[i++] = field;
                field = strtok_r(NULL, ",", &saveptr2);
            }
            if (i >= 7) { 
                cJSON* item = cJSON_CreateObject();
     
                cJSON_AddStringToObject(item, "sFileName", fields[1]);
      
                cJSON_AddNumberToObject(item, "iFileSize", atol(fields[5]));

                char iStartTime[20] = "", sEndTime[20] = "";
                int iDuration = 0;
                printf("DEBUG: fields[2]=[%s], len=%zu\n", fields[2], strlen(fields[2]));
                if (fields[2] && strlen(fields[2]) == 22) {
                    snprintf(iStartTime, sizeof(iStartTime), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                        fields[2], fields[2]+4, fields[2]+6, fields[2]+9, fields[2]+11, fields[2]+13);
                    snprintf(sEndTime, sizeof(sEndTime), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                        fields[2], fields[2]+4, fields[2]+6, fields[2]+16, fields[2]+18, fields[2]+20);
                    struct tm tm1 = {0}, tm2 = {0};
                    strptime(iStartTime, "%Y-%m-%d %H:%M:%S", &tm1);
                    strptime(sEndTime, "%Y-%m-%d %H:%M:%S", &tm2);
                    time_t t1 = mktime(&tm1);
                    time_t t2 = mktime(&tm2);
                    iDuration = (t2 > t1) ? (int)(t2-t1) : 0;
                } else {
                    strncpy(iStartTime, fields[2], sizeof(iStartTime)-1); iStartTime[sizeof(iStartTime)-1]=0;
                    strcpy(sEndTime, "");
                    iDuration = 0;
                }
                printf("DEBUG: iStartTime=[%s], sEndTime=[%s]\n", iStartTime, sEndTime);
                cJSON_AddStringToObject(item, "iStartTime", iStartTime);
                cJSON_AddStringToObject(item, "sEndTime", sEndTime);
                cJSON_AddNumberToObject(item, "iDuration", iDuration);

                unsigned int mask = 0;
                if (fields[6] && strlen(fields[6]) > 0)
                    mask = strtoul(fields[6], NULL, 16);
                printf("[DEBUG] 通道掩码: %s (解析为 0x%X)\n", fields[6], mask);

                int channelArr[8] = {0};
                int channelNum = 0;
                for (int ch = 0; ch < 8; ++ch) { 
                    if ((mask >> ch) & 1) {
                        channelArr[channelNum++] = ch;
                    }
                }
                cJSON* jChannels = cJSON_CreateIntArray(channelArr, channelNum);
                cJSON_AddItemToObject(item, "jChannels", jChannels);
                cJSON_AddNumberToObject(item, "iChannelNum", channelNum);
                cJSON_AddNumberToObject(item, "iChannelMask", mask);

                // sURL
                cJSON_AddStringToObject(item, "sURL", "");

                cJSON_AddItemToArray(items, item);
                count++;
            }
            line_index++;
            line = strtok_r(NULL, ";", &saveptr1);
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "totalRecords", ret);
    cJSON_AddItemToObject(result, "records", items);

    char* jsonStr = cJSON_PrintUnformatted(result);
    printf("DEBUG: generated JSON string (raw) = %s\n", jsonStr);
    cJSON_Delete(result);

    len = strlen(jsonStr);
    if (len > params.bufferLength) len = params.bufferLength;

    printf("SEND TO CLIENT JSON (for debug):\n%s\n", jsonStr);
    printf("SEND JSON length: %d, total ret: %d, 实际发送的条数: %d\n", len, ret, count);

    if (sock_write(fd, &len, sizeof(len)) == SOCKERR_CLOSED) {
        free(jsonStr); free(recordList);
        return -1;
    }
    if (sock_write(fd, jsonStr, len) == SOCKERR_CLOSED) {
        free(jsonStr); free(recordList);
        return -1;
    }
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        free(jsonStr); free(recordList);
        return -1;
    }

    free(jsonStr); free(recordList);
    return 0;
}


int PlayMediaDataCallback(int chn, int stream_type, FRAME_TYPE_E frame_type, int frame_len, int64_t frame_pts, char* frame_buf, void* args)
{
	//printf("Play chn(%d) %ld\n", chn, frame_pts);
	//if (frame_type == REC_FRAME_TYPE_H264I || frame_type == REC_FRAME_TYPE_H264P)
	//	MsNetdvr_Playback_SendEncoderData(chn, frame_len, frame_buf, false);
	return 0;
}

int PlayStatusOutcb(LOCAL_CTRL_CMD_E playStatus, int playSpeed, int playTime, int timeLen)
{
	printf("Play status(%d) %d\n", playStatus, playTime);
	if (playStatus == LOCAL_CTRL_CMD_STOP)
	{
		//for (uint32_t i = 0; i < 9; i++)
		//{
		//	MsNetdvr_Playback_SendEncoderData(i, 0, NULL, true);
		//}
		//MsNetdvr_Vo_Playback_Close(1);
	}
	return 0;
}

int ser_rk_record_file_play(int fd)
{
    PlayBack_t params;
    MS_LOCAL_REC_INFO info;
    memset(&params, 0, sizeof(params));
    memset(&info, 0, sizeof(info));

    // 1. 读取客户端参数
    if (sock_read(fd, &params, sizeof(params)) == SOCKERR_CLOSED) {
        printf("Failed to read params from client\n");
        int fail = -1;
        sock_write(fd, &fail, sizeof(fail));
        return -1;
    }
    printf("params.channel = %d\n", params.channel);
    int ret = rk_record_file_play(&params, &info);

    if (ret == 0) {
        FILE_INFO_T file_info;
        memset(&file_info, 0, sizeof(file_info));

        int fi_ret = MsNetdvr_GetRecordFileInfo(info.fullpath, &file_info);
        if (fi_ret != 0) {
            printf("Failed to get FILE_INFO_T\n");
            ret = -2;
        } else {
            int rtmp_start_ret = ms_rtmp_palyback_start(params.channel, &file_info);
            if (rtmp_start_ret != 0) {
                printf("RTMP playback start failed!\n");
                ret = -3;
            }
        }
    }

    if (sock_write(fd, &info, sizeof(info)) == SOCKERR_CLOSED) {
        printf("Failed to write MS_LOCAL_REC_INFO to client\n");
        return -5;
    }

    return ret;
}

int ser_rk_storage_get_stream_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    printf("Enter called \n");
    RecordStreamConfig_t config;
    int ret = rk_storage_get_stream_config(&config);
    if(sock_write(fd, &config, sizeof(config)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for config failed!\n", __func__);
        return -1;
    }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for config failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_storage_set_stream_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    RecordStreamConfig_t config;
    if(sock_read(fd, &config, sizeof(config)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for config failed!\n", __func__);
        return -1;
    }
    int ret = rk_storage_set_stream_config(&config);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_storage_get_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    StorageConfig_t config;
    int ret = rk_storage_get_config(&config);
    if(sock_write(fd, &config, sizeof(config)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for config failed!\n", __func__);
        return -1;
    }
    // if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
    //     server_printf("[%s] sock_write for config failed!\n", __func__);
    //     return -1;
    // }
    return 0;
}

int ser_rk_storage_set_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    StorageConfig_t config;
    if(sock_read(fd, &config, sizeof(config)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for config failed!\n", __func__);
        return -1;
    }
    int ret = rk_storage_set_config(&config);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_storage_format(int fd) {
    printf("Enter storage_format\n");
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_storage_format();
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_storage_get_disk_format_status(int fd) {
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_storage_get_disk_format_status();
    printf("rk_storage_get_disk_format_status = %d\n", ret);
    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}


// int ser_rk_get_device_info(int fd){
// 	server_printf("[%s] called, fd=%d\n", __func__, fd);
// 	DeviceInfo_t deviceif;
// 	int ret = rk_get_device_info(&deviceif);
// 	if(sock_write(fd, &deviceif, sizeof(deviceif)) == SOCKERR_CLOSED){
// 		server_printf("[%s] sock_write for fileinfo failed!\n", __func__);
// 		return -1;
// 	}
//     if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
// 		server_printf("[%s] sock_write for fileinfo failed!\n", __func__);
// 		return -1;
// 	}
// 	return 0;
// }

int ser_rk_get_running_status(int fd){
	server_printf("[%s] called, fd=%d\n", __func__, fd);
	RunningStatus_t RunStatus;
	int ret = rk_get_running_status(&RunStatus);
	if(sock_write(fd, &RunStatus, sizeof(RunStatus)) == SOCKERR_CLOSED){
		return -1;
	}
	return 0;
}

int ser_rk_get_product_info(int fd){
	server_printf("[%s] called, fd=%d\n", __func__, fd);
	ProductInfo_t proinfo;
	int ret = rk_get_product_info(&proinfo);
	if(sock_write(fd, &proinfo, sizeof(proinfo)) ==SOCKERR_CLOSED){
		return -1;
	}
    if(sock_write(fd, &ret, sizeof(ret)) ==SOCKERR_CLOSED){
		return -1;
	}
	return 0;
}
int ser_rk_set_device_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DeviceConfig_t deviceconfig;
    if(sock_read(fd, &deviceconfig, sizeof(deviceconfig)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for config failed!\n", __func__);
        return -1;
    }
    printf("Received channelEnabled: [%d, %d, %d, %d]\n",
        deviceconfig.ahdConfig.channelEnabled[0],
        deviceconfig.ahdConfig.channelEnabled[1],
        deviceconfig.ahdConfig.channelEnabled[2],
        deviceconfig.ahdConfig.channelEnabled[3]);
    
    int ret = rk_set_device_config(&deviceconfig);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_get_device_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DeviceConfig_t deviceconfig;
    int ret = rk_get_device_config(&deviceconfig);
    if(sock_write(fd, &deviceconfig, sizeof(deviceconfig)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_format_tf_card(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_format_tf_card();
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_driver_delete_info(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_driver_delete_info();
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;  
}

int ser_rk_driver_get_list(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DriverList_t driverlist;

    int ret = rk_driver_get_list(&driverlist);

    // 发送 driverlist.count
    if (sock_write(fd, &driverlist.count, sizeof(driverlist.count)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write for count failed!\n", __func__);
        free(driverlist.drivers); 
        return -1;
    }

    if (driverlist.count > 0 && driverlist.drivers != NULL) {
        printf("Enter driverlist.drivers\n");
        if (sock_write(fd, driverlist.drivers, driverlist.count * sizeof(DriverInfo_t)) == SOCKERR_CLOSED) {
            server_printf("[%s] sock_write for drivers failed!\n", __func__);
            free(driverlist.drivers);  
            return -1;
        }
    }

    if (sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED) {
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        free(driverlist.drivers); 
        return -1;
    }

    if (driverlist.drivers) {
        free(driverlist.drivers);
        driverlist.drivers = NULL;
    }
    return 0;
}


int ser_rk_verify_get_records(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    VerifyRecordList_t verifyrecordlist;
    int ret = rk_verify_get_records(&verifyrecordlist);
    if(sock_write(fd, &verifyrecordlist.count, sizeof(verifyrecordlist.count)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for count failed!\n", __func__);
        return -1;
    }
        if(sock_write(fd, verifyrecordlist.records, verifyrecordlist.count * sizeof(VerifyRecord_t)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for count failed!\n", __func__);
        free(verifyrecordlist.records);
        return -1;
    }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(verifyrecordlist.records){
        free(verifyrecordlist.records);
        verifyrecordlist.records = NULL;
    }
    return 0;
}

int ser_rk_verify_clear_records(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_verify_clear_records();
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_capture_set_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    CaptureConfig_t captureconfig;
    if(sock_read(fd, &captureconfig, sizeof(captureconfig)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for ret failed!\n", __func__);
        return -1;
    }
    int ret = rk_capture_set_config(&captureconfig);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}


int ser_rk_capture_get_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    CaptureConfig_t captureconfig;
    int ret = rk_capture_get_config(&captureconfig);
    if(sock_write(fd, &captureconfig, sizeof(captureconfig)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_get_debug_feature_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DebugFeatureConfig_t cfg;
    int ret = rk_get_debug_feature_config(&cfg);
    if(sock_write(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    return 0;
}

int ser_rk_set_debug_feature_config(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    DebugFeatureConfig_t cfg;
    if(sock_read(fd, &cfg, sizeof(cfg)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_read for ret failed!\n", __func__);
        return -1;
    }
    int ret = rk_set_debug_feature_config(&cfg);
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    
}

int ser_rk_capture_get_records(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    CaptureRecordList_t recordlist;
    int ret = rk_capture_get_records(&recordlist);
    if(sock_write(fd, &recordlist.count, sizeof(recordlist.count)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(sock_write(fd, recordlist.records, recordlist.count * sizeof(CaptureRecord_t)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
    if(recordlist.records){
        free(recordlist.records);
        recordlist.records = NULL;
    }
    return 0;
}

int ser_rk_clear_driver_register_info(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_clear_driver_register_info();
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
}

int ser_rk_capture_clear_records(int fd){
    server_printf("[%s] called, fd=%d\n", __func__, fd);
    int ret = rk_capture_clear_records();
    if(sock_write(fd, &ret, sizeof(ret)) == SOCKERR_CLOSED){
        server_printf("[%s] sock_write for ret failed!\n", __func__);
        return -1;
    }
}

static const struct FunMap map[] = {
    {(char *)"rk_get_incident_images", &ser_rk_get_incident_images},
    {(char *)"rk_create_kit", &ser_rk_create_kit},
    {(char *)"rk_get_current_location", &ser_rk_get_current_location},
    {(char *)"rk_get_health_check", &ser_rk_get_health_check},
    {(char *)"rk_config_device_settings", &ser_rk_config_device_settings},
    {(char *)"rk_create_incident", &ser_rk_create_incident},
	{(char *)"rk_adas_get_preview_config", &ser_rk_adas_get_preview_config},
	{(char *)"rk_adas_set_preview_config", &ser_rk_adas_set_preview_config},
	{(char *)"rk_dms_get_config", &set_rk_dms_get_config},
	{(char *)"rk_dms_set_config", &set_rk_dms_set_config},
	{(char *)"rk_adas_get_config", &ser_rk_adas_get_config},
	{(char *)"rk_adas_set_config", &ser_rk_adas_set_config},
	{(char *)"ms_record_play_control", &ser_ms_record_play_control},
	{(char *)"rk_get_record_file_list", &ser_rk_get_record_file_list},
	{(char *)"rk_storage_get_stream_config", &ser_rk_storage_get_stream_config},
	{(char *)"rk_storage_set_stream_config", &ser_rk_storage_set_stream_config},
	{(char *)"rk_storage_format", &ser_rk_storage_format},
	{(char *)"rk_storage_set_config", &ser_rk_storage_set_config},
	{(char *)"rk_storage_get_config", &ser_rk_storage_get_config},
	{(char *)"rk_get_device_info", &ser_rk_get_device_info},
	{(char *)"rk_get_running_status", &ser_rk_get_running_status},
	{(char *)"rk_get_product_info", &ser_rk_get_product_info},
    {(char *)"rk_set_device_config", &ser_rk_set_device_config},
    {(char *)"rk_get_device_config", &ser_rk_get_device_config},
    {(char *)"rk_format_tf_card", &ser_rk_format_tf_card},
    {(char *)"rk_driver_delete_info", &ser_rk_driver_delete_info},
    {(char *)"rk_driver_get_list", &ser_rk_driver_get_list},
    {(char *)"rk_verify_get_records", &ser_rk_verify_get_records},
    {(char *)"rk_verify_clear_records", &ser_rk_verify_clear_records},
    {(char *)"rk_record_file_play", &ser_rk_record_file_play},
    {(char *)"rk_driver_register", &ser_rk_driver_register},
    {(char *)"rk_capture_get_config", &ser_rk_capture_get_config},
    {(char *)"rk_capture_set_config", &ser_rk_capture_set_config},
    {(char *)"rk_get_debug_feature_config", &ser_rk_get_debug_feature_config},
    {(char *)"rk_set_debug_feature_config", &ser_rk_set_debug_feature_config},
    {(char *)"rk_capture_get_records", &ser_rk_capture_get_records},
    {(char *)"rk_clear_driver_register_info", &ser_rk_clear_driver_register_info},
    {(char *)"rk_capture_clear_records", &ser_rk_capture_clear_records},
    {(char *)"rk_storage_get_disk_format_status", &ser_rk_storage_get_disk_format_status},
    {(char *)"rk_system_upgrade", &ser_rk_system_upgrade},
    {(char *)"rk_system_get_update_type", &ser_rk_system_get_update_type},
    {(char *)"rk_system_get_update_progress", &ser_rk_system_get_update_progress},
};
static void *rec_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    server_printf("[rkipc][THREAD] Start, fd=%d\n", fd);
    char *name = NULL;
    int len;
    int ret = 0;
    int i;
    int maplen = sizeof(map) / sizeof(struct FunMap);
    server_printf("maplen = %d\n", maplen);
    pthread_detach(pthread_self());

    if (sock_write(fd, &ret, sizeof(int)) == SOCKERR_CLOSED)
    {
        server_printf("[rec_thread] sock_write init failed!\n");
        ret = -1;
        goto out;
    }
    else
    {
        server_printf("[rec_thread] socket_write successfully\n");
    }
again:
    if (sock_read(fd, &len, sizeof(int)) == SOCKERR_CLOSED)
    {
        server_printf("[rec_thread] sock_read1 failed\n");
        ret = -1;
        goto out;
    }
    else
    {
        server_printf("[rec_thread] socket_read1 successfully, len=%d\n", len);
    }
    if (len <= 0)
    {
        server_printf("[rec_thread] len <= 0, ret=%d\n", ret);
        ret = -1;
        goto out;
    }
    else
    {
        server_printf("[rec_thread] len > 0\n");
    }
    name = (char *)malloc(len);
    if (!name) {
        server_printf("[rec_thread] malloc name failed, len=%d\n", len);
        ret = -1;
        goto out;
    }
    if (sock_read(fd, name, len) == SOCKERR_CLOSED)
    {
        server_printf("[rec_thread] sock_read2 failed\n");
        ret = -1;
        goto out;
    }
    else
    {
        server_printf("[rec_thread] socket_read2 successfully, name='%.*s'\n", len, name);
    }

    for (i = 0; i < maplen; i++)
    {
        server_printf("[rec_thread] Comparing:\n  map[%d].fun_name='%s' (len=%zu)\n  name='%.*s' (len=%d)\n",
            i, map[i].fun_name, strlen(map[i].fun_name), len, name, len);

        // //server_printf("  map[%d].fun_name hex:", i);
        // for (size_t k = 0; k < strlen(map[i].fun_name) + 1; k++)
        //     //server_printf(" %02x", (unsigned char)map[i].fun_name[k]);
        // //server_printf("\n  name hex:");
        // for (int k = 0; k < len; k++)
        //    // server_printf(" %02x", (unsigned char)name[k]);
        // //server_printf("\n");

        if (!strcmp(map[i].fun_name, name))
        {
            server_printf("[rec_thread] Function matched: %s\n", map[i].fun_name);
            ret = map[i].fun(fd);
        }
    }
out:
    if (name)
        free(name);
    name = NULL;
    if (ret == 0)
    {
        server_printf("[rec_thread] call finished, ret=0, waiting for next command\n");
        sock_write(fd, &ret, sizeof(int));
        goto again;
    }
    server_printf("[rec_thread] Exiting, closing fd=%d\n", fd);
    close(fd);
    pthread_exit(NULL);

    return 0;
}

void *rkipc_server_thread(void *arg)
{
	server_log_init();
    server_printf("[rkipc_server_thread] started, arg:%p\n", arg);
    RkIpcServerRun = 1;
    int clifd;
    prctl(PR_SET_NAME, "rkipc_server_thread", 0, 0, 0);
    pthread_detach(pthread_self());

    if ((listen_fd = serv_listen(CS_PATH)) < 0)
    {
        server_printf("[rkipc_server_thread] listen fail\n");
        exit(1);
    }
    server_printf("[rkipc_server_thread] serv_listen(%s) -> fd=%d\n", CS_PATH, listen_fd);
    server_printf("RkIpcServerRun = %d\n", RkIpcServerRun);
    while (RkIpcServerRun)
    {
        pthread_t thread_id;
        if ((clifd = serv_accept(listen_fd)) < 0)
        {
            server_printf("[rkipc_server_thread] accept fail\n");
        }else{
            server_printf("[rkipc_server_thread] client fd = %d\n", clifd);
        }
        if (clifd >= 0)
        {
            int err = pthread_create(&thread_id, NULL, rec_thread, (void *)(intptr_t)clifd);
            if (err != 0)
            {
                server_printf("[rkipc_server_thread] rec_thread pthread_create failed: %s (%d)\n",
                    strerror(err), err);
            }
            else
            {
                server_printf("[rkipc_server_thread] rec_thread pthread_create succeeded, tid=%lu\n",
                    (unsigned long)thread_id);
            }
        }
    }
    RkIpcServerTid = 0;
    server_printf("[rkipc_server_thread] Exiting\n");
    pthread_exit(NULL);
    return 0;
}

int rkipc_server_deinit(void)
{
    server_printf("[rkipc_server_deinit] called\n");
    RkIpcServerRun = 0;
    usleep(500 * 1000);
    if (RkIpcServerTid == 0)
        server_printf("[rkipc_server_deinit] success\n");
    else
        server_printf("[rkipc_server_deinit] failed\n");

    return 0;
}
#ifdef __cplusplus
}
#endif
