#include "doordvr_export.h"
#include "server/param.h"  
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

extern "C" char *base64_encode(const unsigned char *bin, size_t bin_len, size_t *out_len);

#define SNAP_VENC_BASE   8
#define SNAP_VENC_MAX    256
#define SNAP_RGA_BASE    2
#define SNAP_RGA_MAX     256
#define SNAP_TIMEOUT_MS  1500

typedef struct SnapContext {
    int             vencChn;
    int             rgaChn;
    int             srcChannel;
    PIC_SIZE_E      dstPicSize;

    volatile int    got_one;  
    int             ok;          
    char           *b64;        
    size_t          b64_len;

    pthread_mutex_t lock;
    pthread_cond_t  cond;
} SnapContext;

static SnapContext *g_ctx_by_venc[SNAP_VENC_MAX] = {0};

// ------------------ 回调：直接 base64，不落地 ------------------
static void video_packet_cb(MEDIA_BUFFER mb)
{
    int chn = RK_MPI_MB_GetChannelID(mb);
    size_t sz = RK_MPI_MB_GetSize(mb);
    void *ptr = RK_MPI_MB_GetPtr(mb);

    SnapContext *ctx = NULL;
    if (chn >= 0 && chn < SNAP_VENC_MAX)
        ctx = g_ctx_by_venc[chn];

    if (!ctx) {
        RK_MPI_MB_ReleaseBuffer(mb);
        return;
    }

    int ok = 0;
    char *b64 = NULL;
    size_t b64_len = 0;

    if (ptr && sz > 0) {
        b64 = base64_encode((const unsigned char*)ptr, sz, &b64_len);
        if (b64 && b64_len > 0) ok = 1;
    }

    pthread_mutex_lock(&ctx->lock);
    ctx->ok      = ok;
    ctx->b64     = b64;
    ctx->b64_len = b64_len;
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

    if (RK_MPI_VENC_CreateChn(ctx->vencChn, &venc_attr)) {
        fprintf(stderr, "[SNAP][CH%d] Create VENC[%d] failed\n", ctx->srcChannel, ctx->vencChn);
        return NULL;
    }

    MPP_CHN_S encChn = {0};
    encChn.enModId = RK_ID_VENC;
    encChn.s32ChnId = ctx->vencChn;
    if (RK_MPI_SYS_RegisterOutCb(&encChn, video_packet_cb)) {
        fprintf(stderr, "[SNAP][CH%d] RegisterOutCb failed\n", ctx->srcChannel);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    VENC_RECV_PIC_PARAM_S recvParam;
    recvParam.s32RecvPicNum = 0;
    RK_MPI_VENC_StartRecvFrame(ctx->vencChn, &recvParam);

    ctx->rgaChn = SNAP_RGA_BASE + ctx->srcChannel;
    if (ctx->rgaChn < 0 || ctx->rgaChn >= SNAP_RGA_MAX) {
        fprintf(stderr, "[SNAP][CH%d] invalid RGA ch id %d\n", ctx->srcChannel, ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    RGA_ATTR_S stRgaAttr;
    memset(&stRgaAttr, 0, sizeof(stRgaAttr));
    stRgaAttr.bEnBufPool   = RK_TRUE;
    stRgaAttr.u16BufPoolCnt= 3;
    stRgaAttr.u16Rotaion   = 0;
    stRgaAttr.enFlip       = RGA_FLIP_H;

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

    if (RK_MPI_RGA_CreateChn(ctx->rgaChn, &stRgaAttr)) {
        fprintf(stderr, "[SNAP][CH%d] Create RGA[%d] failed\n", ctx->srcChannel, ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    MPP_CHN_S srcChn = {0}, midChn = {0}, dstChn = {0};
    srcChn.enModId  = RK_ID_VI;
    srcChn.s32ChnId = ctx->srcChannel;
    midChn.enModId  = RK_ID_RGA;
    midChn.s32ChnId = ctx->rgaChn;
    dstChn.enModId  = RK_ID_VENC;
    dstChn.s32ChnId = ctx->vencChn;

    if (RK_MPI_SYS_Bind(&srcChn, &midChn)) {
        fprintf(stderr, "[SNAP][CH%d] Bind VI->RGA failed\n", ctx->srcChannel);
        RK_MPI_RGA_DestroyChn(ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }
    if (RK_MPI_SYS_Bind(&midChn, &dstChn)) {
        fprintf(stderr, "[SNAP][CH%d] Bind RGA->VENC failed\n", ctx->srcChannel);
        RK_MPI_SYS_UnBind(&srcChn, &midChn);
        RK_MPI_RGA_DestroyChn(ctx->rgaChn);
        RK_MPI_VENC_DestroyChn(ctx->vencChn);
        return NULL;
    }

    VENC_JPEG_PARAM_S jpegParam;
    memset(&jpegParam, 0, sizeof(jpegParam));
    jpegParam.u32Qfactor = 80;
    RK_MPI_VENC_SetJpegParam(ctx->vencChn, &jpegParam);

    recvParam.s32RecvPicNum = 1;
    if (RK_MPI_VENC_StartRecvFrame(ctx->vencChn, &recvParam)) {
        fprintf(stderr, "[SNAP][CH%d] StartRecvFrame failed\n", ctx->srcChannel);
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

    RK_MPI_SYS_UnBind(&midChn, &dstChn);
    RK_MPI_SYS_UnBind(&srcChn, &midChn);
    RK_MPI_RGA_DestroyChn(ctx->rgaChn);
    RK_MPI_VENC_DestroyChn(ctx->vencChn);

    return NULL;
}

int Doordvr_Snap_Multi_Base64(const int *channels, int n, PIC_SIZE_E picSize,
                              SnapResult **out_results)
{
    if (!channels || n <= 0 || !out_results) return -1;

    *out_results = NULL;

    pthread_t *tids = (pthread_t*)calloc(n, sizeof(pthread_t));
    SnapContext **ctxs = (SnapContext**)calloc(n, sizeof(SnapContext*));
    if (!tids || !ctxs) {
        free(tids); free(ctxs);
        return -1;
    }

    SnapResult *results = (SnapResult*)calloc(n, sizeof(SnapResult));
    if (!results) {
        free(tids); free(ctxs);
        return -1;
    }

    int ok_threads = 0;
    for (int i = 0; i < n; ++i) {
        int ch = channels[i];
        int vencChn = SNAP_VENC_BASE + ch;

        // 预置结果内容
        results[i].channel = ch;
        results[i].b64     = NULL;
        results[i].b64_len = 0;

        if (vencChn < 0 || vencChn >= SNAP_VENC_MAX) {
            fprintf(stderr, "[SNAP] invalid venc id %d for ch %d\n", vencChn, ch);
            continue;
        }

        SnapContext *ctx = (SnapContext*)calloc(1, sizeof(SnapContext));
        ctx->vencChn    = vencChn;
        ctx->rgaChn     = -1;
        ctx->srcChannel = ch;
        ctx->dstPicSize = picSize;
        ctx->got_one    = 0;
        ctx->ok         = 0;
        ctx->b64        = NULL;
        ctx->b64_len    = 0;
        pthread_mutex_init(&ctx->lock, NULL);
        pthread_cond_init(&ctx->cond, NULL);

        g_ctx_by_venc[vencChn] = ctx;
        ctxs[ok_threads] = ctx;

        ThreadArg *ta = (ThreadArg*)malloc(sizeof(ThreadArg));
        ta->ctx = ctx;

        int ret = pthread_create(&tids[ok_threads], NULL, single_snap_thread, ta);
        if (ret != 0) {
            fprintf(stderr, "[SNAP] pthread_create failed for ch %d: %d\n", ch, ret);
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

        int ch_index = i;
        if (ctxs[i]->ok && ctxs[i]->b64 && ctxs[i]->b64_len > 0) {
            results[ch_index].b64     = ctxs[i]->b64;     /
            results[ch_index].b64_len = ctxs[i]->b64_len;
        } else {
        }

        pthread_mutex_destroy(&ctxs[i]->lock);
        pthread_cond_destroy(&ctxs[i]->cond);
        free(ctxs[i]); 
    }

    free(tids);
    free(ctxs);

    *out_results = results;
    return 0;
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
