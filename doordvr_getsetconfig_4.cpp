int rk_get_incident_images(const IncidentImageRequest_t *request,
                           IncidentImageResponse_t      *response)
{
  if (!request || !response) return -1;

  // ---- 初始化 response ----
  memset(response, 0, sizeof(*response));
  response->isValid = 1;
  response->error   = NULL; // 你的结构里如果是指针，这里设空即可

  // 安全写 error 的小工具：只有当 response->error 非空时才拷贝
  auto safe_set_error = [&](const char* msg) {
    if (msg && response->error) SAFE_STRCPY(response->error, msg);
  };

  // ---- kitID 校验 ----
  char reqKid[128] = {0};
  SAFE_STRCPY(reqKid, request->kitID);
  const char *storedKid = allConfig.AllConfigDef.config.createkitresponsedata.kitID;

  server_printf("GetDeviceInfo: req='%s', stored='%s'", reqKid, storedKid);
  if (!kitid_validate_then_fill(reqKid, storedKid)) {
    server_printf("kitID is not match");
    response->isValid = 0;
    safe_set_error("kitID is not match");
    return 0; // 按你们原逻辑：置 isValid=0 并返回 0
  }

  // ---- 回填 eventID / kitID，并持久化 eventID ----
  SAFE_STRCPY(response->eventID, request->eventID);
  PERSIST_STR(allConfig.AllConfigDef.config.incidentimagerequest.eventID, request->eventID);
  SAFE_STRCPY(response->kitID, storedKid);

  if (response->eventID[0] == '\0' || response->kitID[0] == '\0') {
    response->isValid = 0;
    safe_set_error("eventID or kitID is NULL");
    return 0;
  }

  // ---- 读取 ids：'1..8' -> 映射到 IPC 9..16（去重，最多 8 路）----
  char *ids = allConfig.AllConfigDef.config.createincidentresponse.incidentImageIds;
  char ids_buf[128] = {0};
  if (ids) SAFE_STRCPY(ids_buf, ids);

  int   absChs[8];  // 保存映射后的绝对通道 9..16
  int   userIds[8]; // 保存对应的用户 id 1..8（与 absChs 对应）
  int   abs_cnt = 0;
  bool  used[9] = {false}; // 1..8

  if (ids_buf[0]) {
    const char *delim = ",; \t";
    for (char *p = strtok(ids_buf, delim); p && abs_cnt < 8; p = strtok(nullptr, delim)) {
      int uid = atoi(p);                 // 期望 1..8
      if (uid >= 1 && uid <= 8 && !used[uid]) {
        used[uid]   = true;
        absChs[abs_cnt]  = IPCCHANNELBEGIN + (uid - 1); // 9..16
        userIds[abs_cnt] = uid;                         // 1..8
        ++abs_cnt;
      }
    }
  }

  if (abs_cnt <= 0) {
    server_printf("incidentImageIds empty or invalid; nothing to snap");
    response->isValid = 0;
    safe_set_error("incidentImageIds is empty");
    return 0;
  }

  // ---- 抓拍（主码流）----
  const char *SAVE_DIR   = "/tmp";
  const int   TIMEOUT_MS = 5000;
  const int   QUALITY    = 90;

  CIpCamManage *mgr = CIpCamManage::Instance();
  int rc_snap = mgr->SnapshotJpegMulti(absChs, abs_cnt, /*isMain=*/true,
                                       SAVE_DIR, TIMEOUT_MS, QUALITY);
  if (rc_snap != 0) {
    server_printf("SnapshotJpegMulti failed, rc=%d", rc_snap);
    response->isValid = 0;
    safe_set_error("snapshot failed");
    return 0;
  }

  // ---- 读取 JPEG -> Base64（使用你文件里的工具函数）----
  struct TmpB64 { int absCh; int userId; char* b64; size_t b64_len; } tmp[8];
  for (int i = 0; i < abs_cnt; ++i) {
    tmp[i].absCh   = absChs[i];
    tmp[i].userId  = userIds[i];
    tmp[i].b64     = NULL;
    tmp[i].b64_len = 0;
  }

  const size_t MIN_B64_LEN = 10000; // 保持你之前的阈值
  int  success_idx[8]; int success_cnt = 0;
  char errbuf[256] = {0}; size_t eoff = 0; int bad_cnt = 0;

  for (int i = 0; i < abs_cnt; ++i) {
    char jpg_path[256];
    snprintf(jpg_path, sizeof(jpg_path), "%s/ipc%d_main.jpg", SAVE_DIR, tmp[i].absCh);

    unsigned char *raw = NULL; size_t raw_len = 0;
    int rf = read_file_all(jpg_path, &raw, &raw_len);
    if (rf != 0 || !raw || raw_len == 0) {
      server_printf("snap file missing/empty: absCh=%d path='%s' rf=%d", tmp[i].absCh, jpg_path, rf);
      int n = snprintf(errbuf + eoff,
                       (eoff < sizeof(errbuf)) ? (sizeof(errbuf) - eoff) : 0,
                       (bad_cnt == 0) ? "%d路摄像头异常" : ",%d路摄像头异常",
                       tmp[i].absCh); // 这里日志用 9..16
      if (n > 0) { eoff += (size_t)n; if (eoff >= sizeof(errbuf)) eoff = sizeof(errbuf) - 1; errbuf[eoff] = '\0'; }
      ++bad_cnt;
      continue;
    }

    size_t b64_len = 0;
    char *b64 = base64_encode(raw, raw_len, &b64_len);
    free(raw);

    if (!b64 || b64_len < MIN_B64_LEN) {
      server_printf("base64 too short/null: absCh=%d path='%s' b64_len=%zu", tmp[i].absCh, jpg_path, b64_len);
      if (b64) { free(b64); }
      int n = snprintf(errbuf + eoff,
                       (eoff < sizeof(errbuf)) ? (sizeof(errbuf) - eoff) : 0,
                       (bad_cnt == 0) ? "%d路摄像头异常" : ",%d路摄像头异常",
                       tmp[i].absCh);
      if (n > 0) { eoff += (size_t)n; if (eoff >= sizeof(errbuf)) eoff = sizeof(errbuf) - 1; errbuf[eoff] = '\0'; }
      ++bad_cnt;
      continue;
    }

    tmp[i].b64     = b64;
    tmp[i].b64_len = b64_len;
    success_idx[success_cnt++] = i;
  }

  if (bad_cnt > 0) safe_set_error(errbuf);

  if (success_cnt == 0) {
    for (int i = 0; i < abs_cnt; ++i) if (tmp[i].b64) { free(tmp[i].b64); tmp[i].b64 = NULL; }
    response->imageCount = 0;
    response->isValid = 0;
    if (bad_cnt == 0) safe_set_error("no frame after length filter (<10000)");
    server_printf("No valid frames (all below %zu). Early return. err='%s'\n",
                  MIN_B64_LEN, response->error ? response->error : "(null)");
    return 0;
  }

  // ---- 构造返回；id 改为 1..8 返回给上层 ----
  response->imageCount = success_cnt;
  response->images = (IncidentImageInfo_t*)calloc((size_t)success_cnt, sizeof(*response->images));
  if (!response->images) {
    for (int i = 0; i < abs_cnt; ++i) if (tmp[i].b64) { free(tmp[i].b64); tmp[i].b64 = NULL; }
    response->imageCount = 0;
    response->isValid = 0;
    safe_set_error("alloc images failed");
    server_printf("alloc images failed\n");
    return 0;
  }

  for (int k = 0; k < success_cnt; ++k) {
    int i = success_idx[k];
    response->images[k].id       = tmp[i].userId;           // 1..8
    response->images[k].file     = tmp[i].b64;              // 直接把指针交给上层
    response->images[k].fileSize = (int)tmp[i].b64_len;
    tmp[i].b64 = NULL;                                      // 防止后面再次 free
  }

  // 清理未用到的临时 b64
  for (int i = 0; i < abs_cnt; ++i) {
    if (tmp[i].b64) { free(tmp[i].b64); tmp[i].b64 = NULL; }
  }

  for (int i = 0; i < success_cnt; ++i) {
    server_printf("Image[%d]: id=%d b64_len=%d\n",
                  i+1, response->images[i].id, response->images[i].fileSize);
  }

  // ----（可选）把 base64 反解为 JPEG 存盘，便于排查 ----
  {
    char ts[32];
    { time_t now = time(NULL); struct tm tm_info; localtime_r(&now, &tm_info);
      strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info); }

    bool any_save_failed = false;
    for (int i = 0; i < success_cnt; ++i) {
      char out_path[256];
      // 这里用返回给上层的 id(1..8) 作文件名
      snprintf(out_path, sizeof(out_path),
               "/app/test_jpeg/snap_ch%d_%s.jpg", response->images[i].id, ts);
      int src = save_base64_to_file(response->images[i].file, out_path);
      if (src == 0) {
        server_printf("Saved decoded image: id=%d path='%s' size(b64)=%d\n",
                      response->images[i].id, out_path, response->images[i].fileSize);
      } else {
        any_save_failed = true;
        server_printf("save_base64_to_file failed: id=%d path='%s' err=%d\n",
                      response->images[i].id, out_path, src);
      }
    }
    if (any_save_failed) {
      response->isValid = 0;
      safe_set_error("save decoded image failed");
    }
  }

  // ---- 保存参数（与原逻辑一致）----
  {
    int z = SaveParam();
    if (z < 0) {
      char buf[256];
      snprintf(buf, sizeof(buf), "SaveLocalParam 失败: %d", z);
      DVR_DEBUG("%s", buf);
    }
  }

  return 0;
}

