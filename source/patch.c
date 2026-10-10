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

/* Controles táctiles del HUD casi invisibles (los maneja el pad físico, ver
 * main.c): el motor dibuja cada ítem del HUD con GameGUI::PaintGraphItem y el
 * joystick con Main::RenderJoystick, ambos con el alfa global GameGUI+0x94
 * (16.16). Se baja ese alfa solo mientras se dibujan esos controles. */
#define HUD_ALPHA_DIV 100 /* 1% */

static so_hook h_paint_item, h_render_joy;
static int (*GUILevel_GetParamValue)(void *lvl, int item, int param);

static void PaintGraphItem_hook(uintptr_t gui, int level, int item, int dx, int dy) {
    int *alpha = (int *)(gui + 0x94);
    int old = *alpha, faded = 0;
    if (level == 3) { /* 3 = HUD de juego (GameGUI::PaintIGNormal) */
        void *lvl = *(void **)(*(uintptr_t *)(gui + 0x88) + level * 4);
        int x = GUILevel_GetParamValue(lvl, item, 2) + dx;
        int y = GUILevel_GetParamValue(lvl, item, 3) + dy;
        static uint64_t seen;
        static int seen_areas;
        /* Espada/escudo: esquina inferior derecha (espacio 1280x752 del motor).
         * Quedan visibles los contextuales de esa zona: 20 = botón de focused
         * kill (GameGUI::ShowFatalityButton), 24/25 = saltar (ShowJumpButton). */
        faded = y > 420 && x > 880 && item != 20 && item != 24 && item != 25;
        if (item < 64 && !(seen & (1ULL << item))) {
            seen |= 1ULL << item;
            l_info("[hud] item %d en (%d,%d)%s", item, x, y, faded ? " -> 1%" : "");
        }
        if (!seen_areas) {
            /* Zonas táctiles del HUD (tipo 5: rect p2,p3,p5,p6 y tecla p7, ver
             * GameGUI::GetTouchKey), una vez, para mapear botones físicos. */
            seen_areas = 1;
            for (int i = 0; i < *(int *)((uintptr_t)lvl + 8); i++)
                if (GUILevel_GetParamValue(lvl, i, 0) == 5)
                    l_info("[hud] zona %d: (%d,%d) %dx%d tecla %d", i,
                           GUILevel_GetParamValue(lvl, i, 2), GUILevel_GetParamValue(lvl, i, 3),
                           GUILevel_GetParamValue(lvl, i, 5), GUILevel_GetParamValue(lvl, i, 6),
                           GUILevel_GetParamValue(lvl, i, 7));
        }
    }
    if (faded)
        *alpha = old / HUD_ALPHA_DIV;
    SO_CONTINUE(int, h_paint_item, gui, level, item, dx, dy);
    *alpha = old;
}

/* Centro de una zona táctil (tipo 5) del HUD de juego; 0 si no existe. */
int hud_area_center(uintptr_t gui, int item, int *x, int *y) {
    void *lvl;
    if (!gui || !GUILevel_GetParamValue)
        return 0;
    lvl = *(void **)(*(uintptr_t *)(gui + 0x88) + 3 * 4);
    if (!lvl || item >= *(int *)((uintptr_t)lvl + 8) || GUILevel_GetParamValue(lvl, item, 0) != 5)
        return 0;
    *x = GUILevel_GetParamValue(lvl, item, 2) + GUILevel_GetParamValue(lvl, item, 5) / 2;
    *y = GUILevel_GetParamValue(lvl, item, 3) + GUILevel_GetParamValue(lvl, item, 6) / 2;
    return 1;
}

static void RenderJoystick_hook(uintptr_t main) {
    int *alpha = (int *)(*(uintptr_t *)(main + 0x374) + 0x94);
    int old = *alpha;
    *alpha = old / HUD_ALPHA_DIV;
    SO_CONTINUE(int, h_render_joy, main);
    *alpha = old;
}

void so_patch(void) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> so_patch() patch.c:%d", __LINE__); }
#endif
    // PVRTC a la GPU en vez de decodificar por software (ver glutil.c)
    hook_addr((uintptr_t)so_symbol(&so_mod, "_Z15PVRTCDecompressPKviiiPh"), (uintptr_t)&PVRTCDecompress_hook);

    GUILevel_GetParamValue = (void *)so_symbol(&so_mod, "_ZN8GUILevel13GetParamValueEii");
    h_paint_item = hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN7GameGUI14PaintGraphItemEiiii"),
                             (uintptr_t)&PaintGraphItem_hook);
    h_render_joy = hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN4Main14RenderJoystickEv"),
                             (uintptr_t)&RenderJoystick_hook);
}
