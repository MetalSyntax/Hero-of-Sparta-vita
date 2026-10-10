/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>

void *sceClibMemclr(void *dst, size_t len) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> sceClibMemclr() mem.c:%d", __LINE__); }
#endif
    return sceClibMemset(dst, 0, len);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> mmap() mem.c:%d", __LINE__); }
#endif
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0) {
        return MAP_FAILED;
    }
    void* ret= malloc(length);
    memset(ret, 0, length);
    return ret;
}

int munmap(void *addr, size_t length) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> munmap() mem.c:%d", __LINE__); }
#endif
    if (addr) free(addr);
    return 0;
}
