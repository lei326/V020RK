#include "doordvr_export.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// #include "doordvr_jpeg.h"

// #ifdef __cplusplus
// #if __cplusplus
// extern "C" {
// #endif /* __cplusplus */
// #endif  /* __cplusplus */

#define SNAP_VENC_BASE          8
#define SNAP_VENC_MAX           256   

#define SNAP_RGA_BASE           2
#define SNAP_RGA_MAX            256

#define SNAP_TIMEOUT_MS         1500

#define SNAP_TMP_DIR            "/opt/C807RK"
#define SNAP_ENCODER_CHANNEL    8

#define TEST_ARGB32_YELLOW      0xFFFFFF00
#define TEST_ARGB32_RED         0xFFFF0033
#define TEST_ARGB32_BLUE        0xFF003399
#define TEST_ARGB32_TRANS       0x00000000

// ------------------------------------------------------------
// 抓拍上下文
// ------------------------------------------------------------
typedef struct SnapContext {
    int             vencChn;        
    int             rgaChn;      
    int             srcChannel;  
    PIC_SIZE_E      dstPicSize;  
    volatile int    got_one;        
    char            saved_path[256];
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} SnapContext;

static SnapContext *g_ctx_by_venc[SNAP_VENC_MAX] = {0};

static void now_timestr(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(buf, n, "%Y%m%d_%H%M%S", &tm_info);
}

static bool quit = false;
static void sigterm_handler(int sig)
{
    fprintf(stderr, "signal %d\n", sig);
    quit = true;
}

static void set_argb8888_buffer(RK_U32 *buf, RK_U32 size, RK_U32 color)
{
    for (RK_U32 i = 0; buf && (i < size); i++)
        *(buf + i) = color;
}

int find_mount_point(const char *device, char *mount_point, size_t size)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp))
    {
        char dev[128], mnt[128], fstype[32], opts[256];
        int ret = sscanf(line, "%127s %127s %31s %255s", dev, mnt, fstype, opts);
        if (ret >= 2 && strcmp(dev, device) == 0)
        {
            strncpy(mount_point, mnt, size - 1);
            mount_point[size - 1] = '\0';
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found ? 0 : -1;
}

static int write_file_all(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[SNAP][CB][ERROR] open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            printf("[SNAP][CB][ERROR] write %s failed at %zu: %s\n", path, written, strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) break;
        written += (size_t)n;
    }
    fsync(fd);
    close(fd);
    printf("[SNAP][CB] write: %zu/%zu bytes -> %s\n", written, len, path);
    return (written == len) ? 0 : -1;
}

static void video_packet_cb(MEDIA_BUFFER mb)
{
    int chn = RK_MPI_MB_GetChannelID(mb);
    int mod = RK_MPI_MB_GetModeID(mb);
    size_t sz = RK_MPI_MB_GetSize(mb);
    void *ptr = RK_MPI_MB_GetPtr(mb);

    printf("[SNAP][CB] fired: mode=%d chn=%d size=%zu ts=%lld\n",
           mod, chn, sz, RK_MPI_MB_GetTimestamp(mb));

    SnapContext *ctx = NULL;
    if (chn >= 0 && chn < SNAP_VENC_MAX)
        ctx = g_ctx_by_venc[chn];

    if (!ctx) {
        for (int i = 0; i < SNAP_VENC_MAX; ++i) {
            if (g_ctx_by_venc[i] && (g_ctx_by_venc[i]->vencChn == chn)) {
                ctx = g_ctx_by_venc[i];
                break;
            }
        }
    }

    if (!ctx) {
        char ts[32], path[256];
        now_timestr(ts, sizeof ts);
        snprintf(path, sizeof path, SNAP_TMP_DIR "/snap_fallback_ch%02d_%s.jpeg", chn, ts);

        if (!ptr || sz == 0) {
            printf("[SNAP][CB][ERROR] invalid mb(no ctx): ptr=%p len=%zu\n", ptr, sz);
        } else {
            if (write_file_all(path, ptr, sz) == 0) {
                printf("[SNAP][CB] fallback saved: %s\n", path);
            }
        }
        RK_MPI_MB_ReleaseBuffer(mb);
        return;
    }

    char ts[32];
    now_timestr(ts, sizeof ts);
    snprintf(ctx->saved_path, sizeof(ctx->saved_path),
             SNAP_TMP_DIR "/snap_ch%02d_%s.jpeg", ctx->srcChannel, ts);

    if (!ptr || sz == 0) {
        printf("[SNAP][CB][ERROR] invalid mb: ptr=%p len=%zu\n", ptr, sz);
    } else {
        if (write_file_all(ctx->saved_path, ptr, sz) == 0) {
            printf("[SNAP][CB] saved: %s\n", ctx->saved_path);
            char mount_point[128] = {0};
            if (find_mount_point("/dev/mmcblk2p2", mount_point, sizeof(mount_point)) == 0) {
                char targetPath[256];
                snprintf(targetPath, sizeof(targetPath), "%s/%s", mount_point,
                         strrchr(ctx->saved_path, '/') ? strrchr(ctx->saved_path, '/') + 1 : ctx->saved_path);
                char cmd[600];
                snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", ctx->saved_path, targetPath);
                int ret = system(cmd);
                sync();
                if (ret == 0) {
                    MessageQueue_Send_Process(MSP_CMD_EXPORT_RADAR_FILE_ACK, NULL, 0, SEND_MESSAGE_TO_UI);
                    printf("[SNAP][CB] copied to SD: %s\n", targetPath);
                } else {
                    MessageQueue_Send_Process(MSP_CMD_EXPORT_RADAR_FILE_FAIL, NULL, 0, SEND_MESSAGE_TO_UI);
                    printf("[SNAP][CB] copy to SD failed, ret=%d\n", ret);
                }
            } else {
                printf("[SNAP][CB] /dev/mmcblk2p2 not mounted, skip copy.\n");
            }
        }
    }

    pthread_mutex_lock(&ctx->lock);
    ctx->got_one = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);

    RK_MPI_MB_ReleaseBuffer(mb);
}

typedef struct ThreadArg {
    SnapContext *ctx;
} ThreadArg;

static void* single_snap_thread(void *arg)
{
    ThreadArg *ta = (ThreadArg*)arg;
    SnapContext *ctx = ta->ctx;
    free(ta);

    VIDEO_NORM_E enNorm =
        (SYSTEM_PAL == Hqt_Vivo_GetSystemFormat()) ? VIDEO_ENCODING_MODE_PAL : VIDEO_ENCODING_MODE_NTSC;

    PIC_SIZE_E enPicSize = Hqt_Common_VideoSizeToSize(Hqt_Vivo_GetViMode(ctx->srcChannel));
    SIZE_S stSrcSize, stDstSize;
    SAMPLE_COMM_SYS_GetPicSize(enNorm, enPicSize, &stSrcSize);
    SAMPLE_COMM_SYS_GetPicSize(enNorm, ctx->dstPicSize, &stDstSize);

    VENC_CHN_ATTR_S venc_attr;
    memset(&venc_attr, 0, sizeof(venc_attr));
    venc_attr.stVencAttr.enType = RK_CODEC_TYPE_JPEG;
    venc_attr.stVencAttr.imageType = IMAGE_TYPE_NV12;
    venc_attr.stVencAttr.u32PicWidth   = stSrcSize.u32Width;
    venc_attr.stVencAttr.u32PicHeight  = stSrcSize.u32Height;
    venc_attr.stVencAttr.u32VirWidth   = stSrcSize.u32Width;
    venc_attr.stVencAttr.u32VirHeight  = stSrcSize.u32Height;
    venc_attr.stVencAttr.stAttrJpege.u32ZoomWidth      = stDstSize.u32Width;
    venc_attr.stVencAttr.stAttrJpege.u32ZoomHeight     = stDstSize.u32Height;
    venc_attr.stVencAttr.stAttrJpege.u32ZoomVirWidth   = stDstSize.u32Width;
    venc_attr.stVencAttr.stAttrJpege.u32ZoomVirHeight  = stDstSize.u32Height;
    venc_attr.stVencAttr.enRotation = VENC_ROTATION_0;

    int ret = RK_MPI_VENC_CreateChn(ctx->vencChn, &venc_attr);
    if (ret) {
        printf("[SNAP][CH%d] Create VENC[%d] failed: %d\n", ctx->srcChannel, ctx->vencChn, ret);
        return NULL;
    }

    MPP_CHN_S encChn = {0};
    encChn.enModId = RK_ID_VENC;
    encChn.s32ChnId = ctx->vencChn;
    ret = RK_MPI_SYS_RegisterOutCb(&encChn, video_packet_cb);
    if (ret) {
        printf("[SNAP][CH%d] RegisterOutCb failed: %d\n", ctx->srcChannel, ret);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    VENC_RECV_PIC_PARAM_S recvParam;
    recvParam.s32RecvPicNum = 0;
    RK_MPI_VENC_StartRecvFrame(ctx->vencChn, &recvParam);

    ctx->rgaChn = SNAP_RGA_BASE + ctx->srcChannel;
    if (ctx->rgaChn < 0 || ctx->rgaChn >= SNAP_RGA_MAX) {
        printf("[SNAP][CH%d] invalid RGA ch id %d\n", ctx->srcChannel, ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    RGA_ATTR_S stRgaAttr;
    memset(&stRgaAttr, 0, sizeof(stRgaAttr));
    stRgaAttr.bEnBufPool   = RK_TRUE;
    stRgaAttr.u16BufPoolCnt= 3;
    stRgaAttr.u16Rotaion   = 0;                 // 不旋转
    stRgaAttr.enFlip       = RGA_FLIP_H;        // ★ 水平翻转，修正镜像 ★

    stRgaAttr.stImgIn.u32X         = 0;
    stRgaAttr.stImgIn.u32Y         = 0;
    stRgaAttr.stImgIn.imgType      = IMAGE_TYPE_NV12;
    stRgaAttr.stImgIn.u32Width     = stSrcSize.u32Width;
    stRgaAttr.stImgIn.u32Height    = stSrcSize.u32Height;
    stRgaAttr.stImgIn.u32HorStride = stSrcSize.u32Width;
    stRgaAttr.stImgIn.u32VirStride = stSrcSize.u32Height;

    stRgaAttr.stImgOut.u32X         = 0;
    stRgaAttr.stImgOut.u32Y         = 0;
    stRgaAttr.stImgOut.imgType      = IMAGE_TYPE_NV12;
    stRgaAttr.stImgOut.u32Width     = stSrcSize.u32Width;
    stRgaAttr.stImgOut.u32Height    = stSrcSize.u32Height;
    stRgaAttr.stImgOut.u32HorStride = stSrcSize.u32Width;
    stRgaAttr.stImgOut.u32VirStride = stSrcSize.u32Height;

    ret = RK_MPI_RGA_CreateChn(ctx->rgaChn, &stRgaAttr);
    if (ret) {
        printf("[SNAP][CH%d] Create RGA[%d] failed: %d\n", ctx->srcChannel, ctx->rgaChn, ret);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    // 6) 绑定：VI -> RGA -> VENC
    MPP_CHN_S srcChn = {0}, midChn = {0}, dstChn = {0};
    srcChn.enModId  = RK_ID_VI;
    srcChn.s32ChnId = ctx->srcChannel;

    midChn.enModId  = RK_ID_RGA;
    midChn.s32ChnId = ctx->rgaChn;

    dstChn.enModId  = RK_ID_VENC;
    dstChn.s32ChnId = ctx->vencChn;

    printf("[SNAP][CH%d] bind VI[%d] -> RGA[%d] -> VENC[%d]\n",
           ctx->srcChannel, srcChn.s32ChnId, midChn.s32ChnId, dstChn.s32ChnId);

    ret = RK_MPI_SYS_Bind(&srcChn, &midChn);
    if (ret) {
        printf("[SNAP][CH%d] Bind VI->RGA failed: %d\n", ctx->srcChannel, ret);
        RK_MPI_RGA_DestroyChn(ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }
    ret = RK_MPI_SYS_Bind(&midChn, &dstChn);
    if (ret) {
        printf("[SNAP][CH%d] Bind RGA->VENC failed: %d\n", ctx->srcChannel, ret);
        RK_MPI_SYS_UnBind(&srcChn, &midChn);
        RK_MPI_RGA_DestroyChn(ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    VENC_JPEG_PARAM_S jpegParam;
    memset(&jpegParam, 0, sizeof(jpegParam));
    jpegParam.u32Qfactor = 80; // 1~99，按需调整
    RK_MPI_VENC_SetJpegParam(ctx->vencChn, &jpegParam);

    recvParam.s32RecvPicNum = 1;
    ret = RK_MPI_VENC_StartRecvFrame(ctx->vencChn, &recvParam);
    if (ret) {
        printf("[SNAP][CH%d] StartRecvFrame failed: %d\n", ctx->srcChannel, ret);
        RK_MPI_SYS_UnBind(&midChn, &dstChn);
        RK_MPI_SYS_UnBind(&srcChn, &midChn);
        RK_MPI_RGA_DestroyChn(ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ns_add = (long)SNAP_TIMEOUT_MS * 1000000L;
    ts.tv_sec  += ns_add / 1000000000L;
    ts.tv_nsec += ns_add % 1000000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&ctx->lock);
    while (!ctx->got_one) {
        int wret = pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts);
        if (wret == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!ctx->got_one) {
        printf("[SNAP][CH%d][WARN] timeout no frame.\n", ctx->srcChannel);
    } else {
        printf("[SNAP][CH%d] done: %s\n", ctx->srcChannel, ctx->saved_path);
    }

    RK_MPI_SYS_UnBind(&midChn, &dstChn);
    RK_MPI_SYS_UnBind(&srcChn, &midChn);
    RK_MPI_RGA_DestroyChn(ctx->rgaChn);
    RK_MPI_VENC_DestroyChn(ctx->vencChn);

    return NULL;
}

int Doordvr_Snap_Multi(const int *channels, int n, PIC_SIZE_E picSize)
{
    if (!channels || n <= 0) return -1;
    DVR_DEBUG("[SNAP] multi start, count=%d, size=%d\n", n, picSize);

    pthread_t *tids = (pthread_t*)calloc(n, sizeof(pthread_t));
    SnapContext **ctxs = (SnapContext**)calloc(n, sizeof(SnapContext*));
    if (!tids || !ctxs) { free(tids); free(ctxs); return -1; }

    int ok_threads = 0;
    for (int i = 0; i < n; ++i) {
        int ch = channels[i];
        int vencChn = SNAP_VENC_BASE + ch;
        if (vencChn < 0 || vencChn >= SNAP_VENC_MAX) {
            printf("[SNAP] invalid venc id %d for ch %d\n", vencChn, ch);
            continue;
        }

        SnapContext *ctx = (SnapContext*)calloc(1, sizeof(SnapContext));
        ctx->vencChn    = vencChn;
        ctx->rgaChn     = -1; 
        ctx->srcChannel = ch;
        ctx->dstPicSize = picSize;
        pthread_mutex_init(&ctx->lock, NULL);
        pthread_cond_init(&ctx->cond, NULL);
        ctx->got_one = 0;

        g_ctx_by_venc[vencChn] = ctx;
        ctxs[ok_threads] = ctx;

        ThreadArg *ta = (ThreadArg*)malloc(sizeof(ThreadArg));
        ta->ctx = ctx;

        int ret = pthread_create(&tids[ok_threads], NULL, single_snap_thread, ta);
        if (ret != 0) {
            printf("[SNAP] pthread_create failed for ch %d: %d\n", ch, ret);
            g_ctx_by_venc[vencChn] = NULL;
            pthread_mutex_destroy(&ctx->lock);
            pthread_cond_destroy(&ctx->cond);
            free(ctx);
            free(ta);
            continue;
        }
        ok_threads++;
    }

    for (int i = 0; i < ok_threads; ++i) {
        pthread_join(tids[i], NULL);


        int vencChn = ctxs[i]->vencChn;
        if (vencChn >= 0 && vencChn < SNAP_VENC_MAX && g_ctx_by_venc[vencChn] == ctxs[i])
            g_ctx_by_venc[vencChn] = NULL;

        pthread_mutex_destroy(&ctxs[i]->lock);
        pthread_cond_destroy(&ctxs[i]->cond);
        free(ctxs[i]);
    }

    free(tids);
    free(ctxs);

    return (ok_threads > 0) ? 0 : -1;
}

int Doordvr_Snap_StartJpeg(VI_CHN channel, PIC_SIZE_E picSize, void *param)
{
    RK_S32 s32CamId = 0;
    RK_S32 ret;
    VIDEO_NORM_E enNorm;
    PIC_SIZE_E enPicSize;
    SIZE_S stPicSize;
    SIZE_S stDstPicSize;

    enNorm = (SYSTEM_PAL == Hqt_Vivo_GetSystemFormat()) ? VIDEO_ENCODING_MODE_PAL : VIDEO_ENCODING_MODE_NTSC;

    enPicSize = Hqt_Common_VideoSizeToSize(Hqt_Vivo_GetViMode(channel));
    DVR_DEBUG("enNorm=%d,enPicSize=%d", enNorm, enPicSize);

    SAMPLE_COMM_SYS_GetPicSize(enNorm, enPicSize, &stPicSize);
    DVR_DEBUG("enNorm=%d,channel=%d,enPicSize=%d,stPicSize.u32Width=%d,stPicSize.u32Height=%d",
              enNorm, channel, enPicSize, stPicSize.u32Width, stPicSize.u32Height);

    SAMPLE_COMM_SYS_GetPicSize(enNorm, picSize, &stDstPicSize);
    DVR_DEBUG("enNorm=%d,channel=%d,picSize=%d,stDstPicSize.u32Width=%d,stDstPicSize.u32Height=%d",
              enNorm, channel, picSize, stDstPicSize.u32Width, stDstPicSize.u32Height);

    VENC_CHN_ATTR_S venc_chn_attr;
    memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
    venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_JPEG;
    venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_NV12;
    venc_chn_attr.stVencAttr.u32PicWidth  = stPicSize.u32Width;
    venc_chn_attr.stVencAttr.u32PicHeight = stPicSize.u32Height;
    venc_chn_attr.stVencAttr.u32VirWidth  = stPicSize.u32Width;
    venc_chn_attr.stVencAttr.u32VirHeight = stPicSize.u32Height;
    venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomWidth      = stDstPicSize.u32Width;
    venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomHeight     = stDstPicSize.u32Height;
    venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomVirWidth   = stDstPicSize.u32Width;
    venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomVirHeight  = stDstPicSize.u32Height;
    venc_chn_attr.stVencAttr.enRotation = VENC_ROTATION_0;

    ret = RK_MPI_VENC_CreateChn(SNAP_ENCODER_CHANNEL, &venc_chn_attr);
    if (ret) {
        printf("Create Venc failed! ret=%d\n", ret);
        return -1;
    }

    MPP_CHN_S stEncChn;
    stEncChn.enModId = RK_ID_VENC;
    stEncChn.s32ChnId = SNAP_ENCODER_CHANNEL;
    ret = RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb);
    if (ret) {
        printf("Register Output callback failed! ret=%d\n", ret);
        RK_MPI_VENC_DestroyChn(SNAP_ENCODER_CHANNEL);
        return -1;
    }

    VENC_RECV_PIC_PARAM_S stRecvParam;
    stRecvParam.s32RecvPicNum = 0;
    RK_MPI_VENC_StartRecvFrame(SNAP_ENCODER_CHANNEL, &stRecvParam);

    const int SNAP_RGA_CHANNEL = SNAP_RGA_BASE + channel;
    RGA_ATTR_S stRgaAttr;
    memset(&stRgaAttr, 0, sizeof(stRgaAttr));
    stRgaAttr.bEnBufPool    = RK_TRUE;
    stRgaAttr.u16BufPoolCnt = 3;
    stRgaAttr.u16Rotaion    = 0;          
    stRgaAttr.enFlip        = RGA_FLIP_H; 

    // 输入（来自 VI）
    stRgaAttr.stImgIn.u32X         = 0;
    stRgaAttr.stImgIn.u32Y         = 0;
    stRgaAttr.stImgIn.imgType      = IMAGE_TYPE_NV12;
    stRgaAttr.stImgIn.u32Width     = stPicSize.u32Width;
    stRgaAttr.stImgIn.u32Height    = stPicSize.u32Height;
    stRgaAttr.stImgIn.u32HorStride = stPicSize.u32Width;
    stRgaAttr.stImgIn.u32VirStride = stPicSize.u32Height;

    // 输出（送 VENC）
    stRgaAttr.stImgOut.u32X         = 0;
    stRgaAttr.stImgOut.u32Y         = 0;
    stRgaAttr.stImgOut.imgType      = IMAGE_TYPE_NV12;
    stRgaAttr.stImgOut.u32Width     = stPicSize.u32Width;
    stRgaAttr.stImgOut.u32Height    = stPicSize.u32Height;
    stRgaAttr.stImgOut.u32HorStride = stPicSize.u32Width;
    stRgaAttr.stImgOut.u32VirStride = stPicSize.u32Height;

    ret = RK_MPI_RGA_CreateChn(SNAP_RGA_CHANNEL, &stRgaAttr);
    if (ret) {
        printf("Create RGA failed! ret=%d\n", ret);
        RK_MPI_VENC_DestroyChn(SNAP_ENCODER_CHANNEL);
        return -1;
    }

    MPP_CHN_S stSrcChn, stMidChn, stDestChn;
    stSrcChn.enModId  = RK_ID_VI;
    stSrcChn.s32ChnId = channel;

    stMidChn.enModId  = RK_ID_RGA;
    stMidChn.s32ChnId = SNAP_RGA_CHANNEL;

    stDestChn.enModId  = RK_ID_VENC;
    stDestChn.s32ChnId = SNAP_ENCODER_CHANNEL;

    ret = RK_MPI_SYS_Bind(&stSrcChn, &stMidChn);
    if (ret) {
        printf("Bind VI -> RGA failed! ret=%d\n", ret);
        RK_MPI_RGA_DestroyChn(SNAP_RGA_CHANNEL);
        RK_MPI_VENC_DestroyChn(SNAP_ENCODER_CHANNEL);
        return -1;
    }
    ret = RK_MPI_SYS_Bind(&stMidChn, &stDestChn);
    if (ret) {
        printf("Bind RGA -> VENC failed! ret=%d\n", ret);
        RK_MPI_SYS_UnBind(&stSrcChn, &stMidChn);
        RK_MPI_RGA_DestroyChn(SNAP_RGA_CHANNEL);
        RK_MPI_VENC_DestroyChn(SNAP_ENCODER_CHANNEL);
        return -1;
    }

    int qfactor = 80;
    VENC_JPEG_PARAM_S stJpegParam;
    memset(&stJpegParam, 0, sizeof(stJpegParam));
    stJpegParam.u32Qfactor = qfactor;
    RK_MPI_VENC_SetJpegParam(SNAP_ENCODER_CHANNEL, &stJpegParam);

    stRecvParam.s32RecvPicNum = 1;
    ret = RK_MPI_VENC_StartRecvFrame(SNAP_ENCODER_CHANNEL, &stRecvParam);
    if (ret) {
        printf("RK_MPI_VENC_StartRecvFrame failed!\n");
        RK_MPI_SYS_UnBind(&stMidChn, &stDestChn);
        RK_MPI_SYS_UnBind(&stSrcChn, &stMidChn);
        RK_MPI_RGA_DestroyChn(SNAP_RGA_CHANNEL);
        RK_MPI_VENC_DestroyChn(SNAP_ENCODER_CHANNEL);
        return -1;
    }

    sleep(1);

    RK_MPI_SYS_UnBind(&stMidChn, &stDestChn);
    RK_MPI_SYS_UnBind(&stSrcChn, &stMidChn);
    RK_MPI_RGA_DestroyChn(SNAP_RGA_CHANNEL);
    RK_MPI_VENC_DestroyChn(SNAP_ENCODER_CHANNEL);

    printf("%s exit!\n", __func__);
    return 0;
}

// #ifdef __cplusplus
// #if __cplusplus
// }
// #endif /* __cplusplus */
// #endif  /* __cplusplus */
