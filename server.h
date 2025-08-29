// Copyright 2022 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef __RKIPC_SERVER__
#define __RKIPC_SERVER__
#include <stdio.h>
extern FILE *g_server_log_fp;
#ifdef __cplusplus
extern "C" {
#endif

void server_log_init(void);

#ifdef __cplusplus
}
#endif

#define server_printf(fmt, ...) \
    do { \
        if (g_server_log_fp) { \
            fprintf(g_server_log_fp, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

void CreatePthread_server(void);
int rkipc_server_init(void);
int rkipc_server_deinit(void);
extern int SearchRecordFiles(); 
int SearchRecordPage(
    int channel,
    time_t starttime,
    time_t endtime,
    int recordtype,
    const char *searchDir,
    int maxperpage,
    int currentPage,
    int searchType,
    char *RecData,
    int dataSize
);
#endif
