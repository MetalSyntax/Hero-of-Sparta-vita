/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

/* Estándar de logs del toolkit (PORTING_PLAN.md §6):
 * <DATA_PATH>logs/heroofsparta_NNN.log, 001..999 con next.idx.
 *
 * Backend con sceIo* directo (sin stdio): el log tiene que existir incluso si
 * newlib/SceLibc todavía no está listo o si el juego muere antes del primer
 * flush. Si la apertura falla, se reintenta en cada llamada (barato: un
 * sceIoOpen cuando no hay fd válido). */
#define HOS_LOG_TOP DATA_PATH "heroofsparta"
#define HOS_LOG_DIR DATA_PATH "logs"
#define HOS_LOG_SLUG "heroofsparta"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);
static SceUID log_fd = -1;
static int log_open_attempted = 0;
/* Cada línea al archivo lleva ~+1 write; en Debug se hace flush (write es
 * directo, sin buffer) siempre. En Release solo WARN+. */
static char log_line[2200];

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffer C is used for file output (no colors).
static char buffer_c[2048];
static char buffer_d[2048];

static int hos_read_next_idx(void) {
    char idx_path[128];
    SceUID fd;
    char tmp[16];
    int n, next = 1, val = 0, neg = 0, i = 0;

    snprintf(idx_path, sizeof(idx_path), HOS_LOG_DIR "/next.idx");
    fd = sceIoOpen(idx_path, SCE_O_RDONLY, 0777);
    if (fd < 0)
        return 1;
    n = (int)sceIoRead(fd, tmp, sizeof(tmp) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return 1;
    tmp[n] = '\0';
    while (tmp[i] == ' ' || tmp[i] == '\t' || tmp[i] == '\n' || tmp[i] == '\r')
        i++;
    if (tmp[i] == '-') {
        neg = 1;
        i++;
    }
    while (tmp[i] >= '0' && tmp[i] <= '9') {
        val = val * 10 + (tmp[i] - '0');
        i++;
    }
    (void)neg;
    next = val;
    if (next < 1 || next > 999)
        next = 1;
    return next;
}

static void hos_write_next_idx(int next) {
    char idx_path[128];
    char tmp[16];
    SceUID fd;

    snprintf(idx_path, sizeof(idx_path), HOS_LOG_DIR "/next.idx");
    snprintf(tmp, sizeof(tmp), "%d\n", next);
    fd = sceIoOpen(idx_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, tmp, strlen(tmp));
    sceIoClose(fd);
}

static int hos_path_exists(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (fd < 0)
        return 0;
    sceIoClose(fd);
    return 1;
}

static void hos_log_open(void) {
    char log_path[160];
    int next = hos_read_next_idx();
    int slot = -1;

    /* Sin trailing slash: sceIoMkdir es más predecible así. */
    sceIoMkdir("ux0:data/heroofsparta", 0777);
    sceIoMkdir("ux0:data/heroofsparta/logs", 0777);
    sceIoMkdir("ux0:data/heroofsparta/saves", 0777);
    sceIoMkdir("ux0:data/heroofsparta/hos", 0777);

    for (int tries = 0; tries < 1000; tries++) {
        snprintf(log_path, sizeof(log_path), HOS_LOG_DIR "/%s_%03d.log",
                 HOS_LOG_SLUG, next);
        if (!hos_path_exists(log_path)) {
            slot = next;
            break;
        }
        next = next % 999 + 1;
    }
    if (slot < 0)
        return;

    snprintf(log_path, sizeof(log_path), HOS_LOG_DIR "/%s_%03d.log",
             HOS_LOG_SLUG, slot);
    log_fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (log_fd >= 0)
        hos_write_next_idx(slot % 999 + 1);
    (void)HOS_LOG_TOP;
}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    if (log_fd < 0 && !log_open_attempted) {
        log_open_attempted = 1;
        hos_log_open();
    }
    /* Si la primera apertura falló (ux0 ocupado, etc.), reintentar de a poco:
     * cada 256 líneas se permite un reintento. */
    {
        static unsigned call_count = 0;
        if (log_fd < 0 && (++call_count % 256) == 0)
            hos_log_open();
    }

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n", COLOR_PINK, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " • debug    %s\n", fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n", COLOR_BLUE, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ℹ info     %s\n", fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n", COLOR_ORANGE, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ⚠ warning  %s\n", fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n", COLOR_RED, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ⨯ error    %s\n", fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n", COLOR_RED, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ! fatal    %s\n", fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n", COLOR_GREEN, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ! success  %s\n", fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n", COLOR_CYAN, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " … waiting  %s\n", fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;

    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);

    va_start(list, fmt);
    sceClibVsnprintf(buffer_d, sizeof(buffer_d), buffer_c, list);
    va_end(list);

    sceClibPrintf(buffer_b);

    if (log_fd >= 0) {
        int len;
        sceClibSnprintf(log_line, sizeof(log_line), "%s", buffer_d);
        len = (int)strlen(log_line);
        /* sceIoWrite es directo a disco (sin buffer de stdio): cada línea
         * queda registrada aunque el juego cuelgue o crashee enseguida. */
        if (len > 0)
            sceIoWrite(log_fd, log_line, len);
    }

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
