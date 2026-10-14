/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

// Rutas que el .so conoce (ver strings de libheroofsparta.so + jadx):
//   /mnt/sdcard/gameloft/games/hos/...  -> jogo + audio + saves (g_appPath)
//   /sdcard/gameloft/games/hos/...      -> alias del anterior (GameInstaller)
//   /data/data/com.gameloft.android.GAND.GloftSPAW.ML/*.dat -> saves
// En Vita todo vive bajo DATA_PATH:
//   DATA_PATH hos/...    (pack de datos + .ogg + intro.vdo)
//   DATA_PATH saves/...  (Setting.dat, Spartan*.dat, .nomedia)
#define HOS_SDCARD_PREFIX "/mnt/sdcard/gameloft/games/hos/"
#define HOS_SDCARD_PREFIX2 "/sdcard/gameloft/games/hos/"
#define HOS_PRIV_PREFIX "/data/data/com.gameloft.android.GAND.GloftSPAW.ML/"

static const char *hos_translate_path(const char *path, char *out, size_t out_size) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_translate_path() io.c:%d", __LINE__); }
#endif
    if (!path || !out || out_size == 0)
        return path;
    if (strncmp(path, HOS_SDCARD_PREFIX, sizeof(HOS_SDCARD_PREFIX) - 1) == 0) {
        snprintf(out, out_size, DATA_PATH "hos/%s", path + sizeof(HOS_SDCARD_PREFIX) - 1);
        return out;
    }
    if (strncmp(path, HOS_SDCARD_PREFIX2, sizeof(HOS_SDCARD_PREFIX2) - 1) == 0) {
        snprintf(out, out_size, DATA_PATH "hos/%s", path + sizeof(HOS_SDCARD_PREFIX2) - 1);
        return out;
    }
    if (strncmp(path, HOS_PRIV_PREFIX, sizeof(HOS_PRIV_PREFIX) - 1) == 0) {
        snprintf(out, out_size, DATA_PATH "saves/%s", path + sizeof(HOS_PRIV_PREFIX) - 1);
        return out;
    }
    // g_appPath a veces viene sin trailing slash + nombre pegado.
    if (strcmp(path, "/mnt/sdcard/gameloft/games/hos") == 0 ||
        strcmp(path, "/sdcard/gameloft/games/hos") == 0) {
        snprintf(out, out_size, DATA_PATH "hos");
        return out;
    }
    return path;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> fopen_soloader() io.c:%d", __LINE__); }
#endif
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    char tpath[1024];
    const char *real = hos_translate_path(filename, tpath, sizeof(tpath));
    if (real != filename)
        l_debug("fopen redirect: %s -> %s", filename, real);

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(real, mode);
#else
    FILE* ret = fopen(real, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> open_soloader() io.c:%d", __LINE__); }
#endif
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    char tpath[1024];
    const char *real = hos_translate_path(path, tpath, sizeof(tpath));
    if (real != path)
        l_debug("open redirect: %s -> %s", path, real);

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(real, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> fstat_soloader() io.c:%d", __LINE__); }
#endif
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> stat_soloader() io.c:%d", __LINE__); }
#endif
    char tpath[1024];
    const char *real = hos_translate_path(path, tpath, sizeof(tpath));
    struct stat st;
    int res = stat(real, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> fclose_soloader() io.c:%d", __LINE__); }
#endif
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> close_soloader() io.c:%d", __LINE__); }
#endif
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

/* Wrappers con timing (bug #1, lección de Asphalt-5): discriminan lectura
 * lenta de ux0 vs parseo pesado dentro del motor. Solo las lecturas >200ms
 * suben a warning (siempre visible). */
size_t fread_soloader(void *ptr, size_t size, size_t count, FILE *stream) {
#ifdef DEBUG_SOLOADER
    uint32_t t0 = sceKernelGetProcessTimeLow();
#endif
    size_t ret;
    extern volatile unsigned g_io_ops;
#ifdef USE_SCELIBC_IO
    ret = sceLibcBridge_fread(ptr, size, count, stream);
#else
    ret = fread(ptr, size, count, stream);
#endif
    g_io_ops++;
#ifdef DEBUG_SOLOADER
    {
        uint32_t dt = sceKernelGetProcessTimeLow() - t0;
        size_t bytes = ret * size;
        /* Solo las lentas: loguear cada fread (síncrono a ux0) frenaba las cargas. */
        if (dt > 200000)
            l_warn("[io] fread lento: %u bytes en %ums (%p)", (unsigned)bytes,
                   (unsigned)(dt / 1000), stream);
    }
#endif
    return ret;
}

int fseek_soloader(FILE *stream, long offset, int whence) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fseek(stream, offset, whence);
#else
    return fseek(stream, offset, whence);
#endif
}

long ftell_soloader(FILE *stream) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ftell(stream);
#else
    return ftell(stream);
#endif
}

DIR* opendir_soloader(char* _pathname) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> opendir_soloader() io.c:%d", __LINE__); }
#endif
    char tpath[1024];
    const char *real = hos_translate_path(_pathname, tpath, sizeof(tpath));
    DIR* ret = opendir(real);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> closedir_soloader() io.c:%d", __LINE__); }
#endif
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> fcntl_soloader() io.c:%d", __LINE__); }
#endif
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> ioctl_soloader() io.c:%d", __LINE__); }
#endif
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> fsync_soloader() io.c:%d", __LINE__); }
#endif
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
