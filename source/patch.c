/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include "utils/glutil.h"
#include "utils/logger.h"

#include <stdint.h>

extern so_module so_mod;

/* Acceso a las zonas táctiles del GUI (ítems tipo 5: rect p2,p3,p5,p6 y tecla
 * p7, ver GameGUI::GetTouchKey) para que el pad físico toque donde corresponde. */
static int (*GUILevel_GetParamValue)(void *lvl, int item, int param);

static void *gui_level(uintptr_t gui, int level) {
    if (!gui || !GUILevel_GetParamValue || !*(uintptr_t *)(gui + 0x88))
        return NULL;
    return *(void **)(*(uintptr_t *)(gui + 0x88) + level * 4);
}

/* Centro de la zona táctil `item` del nivel GUI `level`; 0 si no existe. */
int hud_area_center(uintptr_t gui, int level, int item, int *x, int *y) {
    void *lvl = gui_level(gui, level);
    if (!lvl || item >= *(int *)((uintptr_t)lvl + 8) || GUILevel_GetParamValue(lvl, item, 0) != 5)
        return 0;
    *x = GUILevel_GetParamValue(lvl, item, 2) + GUILevel_GetParamValue(lvl, item, 5) / 2;
    *y = GUILevel_GetParamValue(lvl, item, 3) + GUILevel_GetParamValue(lvl, item, 6) / 2;
    return 1;
}

void hud_dump_areas(uintptr_t gui, int level) {
    void *lvl = gui_level(gui, level);
    if (!lvl)
        return;
    for (int i = 0; i < *(int *)((uintptr_t)lvl + 8); i++)
        if (GUILevel_GetParamValue(lvl, i, 0) == 5)
            l_info("[hud] nivel %d zona %d: (%d,%d) %dx%d tecla %d", level, i,
                   GUILevel_GetParamValue(lvl, i, 2), GUILevel_GetParamValue(lvl, i, 3),
                   GUILevel_GetParamValue(lvl, i, 5), GUILevel_GetParamValue(lvl, i, 6),
                   GUILevel_GetParamValue(lvl, i, 7));
}

void so_patch(void) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> so_patch() patch.c:%d", __LINE__); }
#endif
    // PVRTC a la GPU en vez de decodificar por software (ver glutil.c)
    hook_addr((uintptr_t)so_symbol(&so_mod, "_Z15PVRTCDecompressPKviiiPh"), (uintptr_t)&PVRTCDecompress_hook);

    GUILevel_GetParamValue = (void *)so_symbol(&so_mod, "_ZN8GUILevel13GetParamValueEii");
}
