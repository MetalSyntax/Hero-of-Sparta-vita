/*
 * main.c -- Hero of Sparta (Gameloft, libheroofsparta.so) en PS Vita.
 *
 * Secuencia replicada de GameRenderer.onSurfaceCreated (GameRenderer.java):
 *   nativeGetJNIEnv -> GLMediaPlayer.nativeInit -> HeroOfSparta.nativeInit(0)
 *   -> GameRenderer.nativeInit(1, w, h) -> loop nativeRender.
 * libStormGLOFT.so NO se carga: es hook anti-tamper sin exports JNI
 * (conclusión del port hermano Dungeon-Hunter-2 + JNI_OnLoad propio
 * desensamblado: solo hace dlopen+hook de glTexImage2D/glViewport/
 * glScissor/glGetError sobre el .so del juego).
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "audio.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 32 * 1024 * 1024;
#endif

so_module so_mod;

/* Requerido por lib/falso_jni/FalsoJNI_Logger.c */
void game_log(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    l_info("%s", buf);
}

/* Punteros a los Java_* de libheroofsparta.so (ver nm -D). */
static void (* nativeGetJNIEnv)(JNIEnv *env, jobject obj);
static void (* nativeGLMediaPlayerInit)(JNIEnv *env, jobject obj);
static void (* nativeGLResLoaderInit)(JNIEnv *env, jobject obj, jint i);
static void (* nativeHeroInit)(JNIEnv *env, jobject obj, jint i);
static void (* nativeRendererInit)(JNIEnv *env, jobject obj, jint a, jint w, jint h);
static void (* nativeRender)(JNIEnv *env, jobject obj);
static void (* nativeResize)(JNIEnv *env, jobject obj, jint w, jint h);
static void (* nativeDone)(JNIEnv *env, jobject obj);
static void (* nativePause)(JNIEnv *env, jobject obj);
static void (* nativeResume)(JNIEnv *env, jobject obj);
static void (* nativeOnTouch)(JNIEnv *env, jobject obj, jint id, jint type, jint x, jint y);
static void (* nativeKeyDown)(JNIEnv *env, jobject obj, jint keycode);
static void (* nativeKeyUp)(JNIEnv *env, jobject obj, jint keycode);
static int (* nativeCanInterrupt)(JNIEnv *env, jobject obj);


/* Keycodes Android (KeyEvent). */
/* appKeyPressed solo alimenta l_pause (appUpdate: 4 = BACK y 82 = MENU abren la
 * pausa); el resto de las teclas el motor no las usa. Por eso solo START va como
 * tecla y todo lo jugable se inyecta como toques sobre los controles del HUD. */
#define AKEYCODE_MENU 82

/* IDs de puntero para los toques sintéticos: el motor asigna sus 10 slots por ID
 * (Main::getTouchIdx), así que van lejos de los IDs de los dedos reales (0-63). */
#define TOUCH_ID_JOY    100
#define TOUCH_ID_SWORD  101
#define TOUCH_ID_SHIELD 102
#define TOUCH_ID_KILL   103
#define TOUCH_ID_CLOSE  104

/* Niveles GUI y zonas táctiles (ítems tipo 5) leídos del motor en vivo; números
 * confirmados con el log de zonas (`[hud] nivel N zona M`). */
#define GUI_HUD           3  /* HUD de juego (GameGUI::PaintIGNormal) */
#define GUI_INFOBOX       13 /* cuadro de texto (UpdateInfoBox: CheckActions(.., 0xd, 0)) */
#define ZONE_FOCUSED_KILL 2  /* tecla 25, botón de ShowFatalityButton */
#define ZONE_SHIELD       4  /* tecla 0 */
#define ZONE_SWORD        6  /* tecla 16 */
#define ZONE_INFOBOX_OK   0  /* la ✕ del cuadro de texto */

/* Desplazamiento del joystick: debe quedar dentro del radio de 100 px con que
 * Main::ProcessJoystick acepta el toque. */
#define JOY_RADIUS 60
#define STICK_DEADZONE 32

static uintptr_t *p_canvas; /* &m_canvas del .so (Main*) */

int hud_area_center(uintptr_t gui, int level, int item, int *x, int *y); /* patch.c */
void hud_dump_areas(uintptr_t gui, int level);

static void touch(int id, int type, int x, int y) {
    if (nativeOnTouch)
        nativeOnTouch(&jni, NULL, id, type, x, y);
}

static uintptr_t cur_gui(void) {
    return (p_canvas && *p_canvas) ? *(uintptr_t *)(*p_canvas + 0x374) : 0;
}

/* Botón físico -> toque mantenido en el centro de una zona táctil del GUI. */
static int zone_pos[8][2];
static unsigned zone_held;

static void zone_down(int id, int level, int zone) {
    int k = id - TOUCH_ID_JOY;
    if (hud_area_center(cur_gui(), level, zone, &zone_pos[k][0], &zone_pos[k][1])) {
        touch(id, 1, zone_pos[k][0], zone_pos[k][1]);
        zone_held |= 1u << k;
        l_debug("pad -> toque %d en nivel %d zona %d (%d,%d)", id, level, zone,
                zone_pos[k][0], zone_pos[k][1]);
    }
}

static void zone_up(int id) {
    int k = id - TOUCH_ID_JOY;
    if (zone_held & (1u << k)) {
        touch(id, 0, zone_pos[k][0], zone_pos[k][1]);
        zone_held &= ~(1u << k);
    }
}

static void pad_buttons(unsigned pressed, unsigned released) {
    uintptr_t gui = cur_gui();
    int infobox = gui ? *(int *)(gui + 0x1c8) : 0;
    static int last_infobox;

    if (infobox != last_infobox) {
        l_info("[hud] cuadro de texto: estado %d tipo %d", infobox, gui ? *(int *)(gui + 0x1f0) : -1);
        if (infobox && !last_infobox)
            hud_dump_areas(gui, GUI_INFOBOX);
        last_infobox = infobox;
    }

    /* CROSS cierra el cuadro de texto (su ✕) si hay uno abierto; si no, espada. */
    if (pressed & SCE_CTRL_CROSS) {
        if (infobox)
            zone_down(TOUCH_ID_CLOSE, GUI_INFOBOX, ZONE_INFOBOX_OK);
        else
            zone_down(TOUCH_ID_SWORD, GUI_HUD, ZONE_SWORD);
    }
    if (released & SCE_CTRL_CROSS) {
        zone_up(TOUCH_ID_CLOSE);
        zone_up(TOUCH_ID_SWORD);
    }
    if (pressed & SCE_CTRL_CIRCLE)
        zone_down(TOUCH_ID_SHIELD, GUI_HUD, ZONE_SHIELD);
    if (released & SCE_CTRL_CIRCLE)
        zone_up(TOUCH_ID_SHIELD);
    if (pressed & SCE_CTRL_TRIANGLE)
        zone_down(TOUCH_ID_KILL, GUI_HUD, ZONE_FOCUSED_KILL);
    if (released & SCE_CTRL_TRIANGLE)
        zone_up(TOUCH_ID_KILL);
}

/* Panel frontal -> un puntero por dedo, como HeroOfSparta.onTouchEvent
 * (tipos 1 = down, 2 = move, 0 = up), en el espacio 1280x752 del motor. */
static void front_touch(const SceTouchData *t) {
    static struct { int id, x, y; } prev[SCE_TOUCH_MAX_REPORT];
    static int nprev;
    int ncur = t->reportNum < SCE_TOUCH_MAX_REPORT ? t->reportNum : SCE_TOUCH_MAX_REPORT;
    int cur_id[SCE_TOUCH_MAX_REPORT], cur_x[SCE_TOUCH_MAX_REPORT], cur_y[SCE_TOUCH_MAX_REPORT];

    for (int i = 0; i < ncur; i++) {
        cur_id[i] = t->report[i].id & 63;
        cur_x[i] = (int)t->report[i].x * ENGINE_W / 1920;
        cur_y[i] = (int)t->report[i].y * ENGINE_H / 1088;
    }
    for (int p = 0; p < nprev; p++) {
        int alive = 0;
        for (int i = 0; i < ncur; i++)
            alive |= cur_id[i] == prev[p].id;
        if (!alive) {
            touch(prev[p].id, 0, prev[p].x, prev[p].y);
            l_debug("touch UP id=%d (%d,%d)", prev[p].id, prev[p].x, prev[p].y);
        }
    }
    for (int i = 0; i < ncur; i++) {
        int p = 0;
        while (p < nprev && prev[p].id != cur_id[i])
            p++;
        if (p == nprev) {
            touch(cur_id[i], 1, cur_x[i], cur_y[i]);
            l_debug("touch DOWN id=%d (%d,%d)", cur_id[i], cur_x[i], cur_y[i]);
        } else if (prev[p].x != cur_x[i] || prev[p].y != cur_y[i]) {
            touch(cur_id[i], 2, cur_x[i], cur_y[i]);
        }
    }
    for (int i = 0; i < ncur; i++) {
        prev[i].id = cur_id[i];
        prev[i].x = cur_x[i];
        prev[i].y = cur_y[i];
    }
    nprev = ncur;
}

/* Cruceta / sticks -> arrastre sobre el joystick virtual. */
static void pad_joystick(const SceCtrlData *pad) {
    static int down;
    int dx = 0, dy = 0, cx, cy;
    uintptr_t gui;

    if (pad->buttons & SCE_CTRL_LEFT)  dx = -128;
    if (pad->buttons & SCE_CTRL_RIGHT) dx = 127;
    if (pad->buttons & SCE_CTRL_UP)    dy = -128;
    if (pad->buttons & SCE_CTRL_DOWN)  dy = 127;
    if (!dx && !dy) {
        dx = pad->lx - 128;
        dy = pad->ly - 128;
        if (dx * dx + dy * dy < STICK_DEADZONE * STICK_DEADZONE) {
            dx = pad->rx - 128;
            dy = pad->ry - 128;
        }
        if (dx * dx + dy * dy < STICK_DEADZONE * STICK_DEADZONE)
            dx = dy = 0;
    }

    if (!p_canvas || !*p_canvas || !(gui = *(uintptr_t *)(*p_canvas + 0x374)))
        return;
    /* Centro del joystick según Main::ProcessJoystick: offsets del GUI + (125,545). */
    cx = *(int *)(gui + 0x18) + 125;
    cy = *(int *)(gui + 0x28) + 545;

    if (dx || dy) {
        if (!down) {
            touch(TOUCH_ID_JOY, 1, cx, cy);
            down = 1;
        }
        touch(TOUCH_ID_JOY, 2, cx + dx * JOY_RADIUS / 128, cy + dy * JOY_RADIUS / 128);
    } else if (down) {
        touch(TOUCH_ID_JOY, 0, cx, cy);
        down = 0;
    }
}

static void *so_sym_or_warn(const char *name) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> so_sym_or_warn() main.c:%d", __LINE__); }
#endif
    void *addr = (void *)so_symbol(&so_mod, name);
    if (!addr)
        l_warn("símbolo no encontrado: %s", name);
    return addr;
}

/* Diagnóstico de cuelgue (bug #1): el log 001 mostró el primer nativeRender()
 * sin retorno. Estos contadores distinguen "loop no avanza" de "atascado DENTRO
 * de nativeRender". El watchdog fuerza un crash controlado si el mismo
 * nativeRender no vuelve en 30s: el .psp2dmp resultante trae los stacks reales
 * de todos los hilos (ver con `psvita-toolkit analyze`). */
volatile int g_render_entered = 0;
volatile int g_render_done = 0;
/* Contador de IO del motor (lo incrementa fread_soloader). Si avanza, el
 * motor trabaja aunque no complete frames (carga lenta legítima). */
volatile unsigned g_io_ops = 0;

static int watchdog_thread(SceSize args, void *argp) {
    int last_done = -1;
    unsigned last_io = 0;
    int stuck_secs = 0;
    (void)args;
    (void)argp;
    while (1) {
        sceKernelDelayThread(1000 * 1000);
        if (!g_render_entered)
            continue; /* loop aún no arrancó */
        if (g_render_done == last_done && g_io_ops == last_io) {
            stuck_secs++;
            if ((stuck_secs % 5) == 0)
                l_warn("[watchdog] sin progreso hace %ds (entered=%d done=%d io=%u)",
                       stuck_secs, g_render_entered, g_render_done, g_io_ops);
            if (stuck_secs >= 60) {
                l_error("[watchdog] 60s sin frames NI lecturas -- crash "
                        "forzado para capturar .psp2dmp con el stack real");
                sceKernelDelayThread(300 * 1000);
                *(volatile int *)0 = 0xDEAD;
            }
        } else {
            if (stuck_secs >= 5)
                l_warn("[watchdog] progreso reanudado (done=%d io=%u)",
                       g_render_done, g_io_ops);
            last_done = g_render_done;
            last_io = g_io_ops;
            stuck_secs = 0;
        }
    }
    return 0;
}

int main() {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> main() main.c:%d", __LINE__); }
#endif
    /* Banner siempre visible (l_error compila en Debug y Release): si este
     * log existe, el loader arrancó; si no existe, el fallo está antes. */
    l_error("[boot] Hero of Sparta Vita -- main() entered (DATA_PATH=" DATA_PATH ")");
    soloader_init_all();

    l_success("resolviendo entry points de libheroofsparta.so...");
    nativeGetJNIEnv = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeGetJNIEnv");
    nativeGLMediaPlayerInit = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GLMediaPlayer_nativeInit");
    nativeGLResLoaderInit = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GLResLoader_nativeInit");
    nativeHeroInit = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeInit");
    nativeRendererInit = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeInit");
    nativeRender = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeRender");
    nativeResize = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeResize");
    nativeDone = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameRenderer_nativeDone");
    nativePause = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameGLSurfaceView_nativePause");
    nativeResume = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_GameGLSurfaceView_nativeResume");
    nativeOnTouch = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeOnTouch");
    nativeKeyDown = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeSetOnKeyDown");
    nativeKeyUp = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeSetOnKeyUp");
    nativeCanInterrupt = so_sym_or_warn(
        "Java_com_gameloft_android_GAND_GloftSPAW_ML_HeroOfSparta_nativeCanInterrupt");

    /* Sin JNI_OnLoad en este .so (nm -D lo confirma): no hay nada que llamar. */
    p_canvas = so_sym_or_warn("m_canvas");

    gl_init();
    l_success("vitaGL inicializado.");

    audio_init();

    /* onSurfaceCreated: el orden importa (los nativeInit cachean IDs JNI). */
    if (nativeGetJNIEnv)
        nativeGetJNIEnv(&jni, NULL);
    else {
        l_fatal("sin nativeGetJNIEnv no hay mEnv: no se puede continuar.");
        sceKernelExitDeleteThread(0);
    }
    if (nativeGLMediaPlayerInit)
        nativeGLMediaPlayerInit(&jni, NULL);
    if (nativeGLResLoaderInit)
        nativeGLResLoaderInit(&jni, NULL, 0);
    if (nativeHeroInit)
        nativeHeroInit(&jni, NULL, 0);
    l_success("inits JNI listos.");
    l_error("[boot] fase: inits JNI OK");

    if (nativeRendererInit)
        nativeRendererInit(&jni, NULL, 1, ENGINE_W, ENGINE_H);
    else {
        l_fatal("sin nativeInit no hay appInit: no se puede continuar.");
        sceKernelExitDeleteThread(0);
    }
    l_success("appInit OK.");
    l_error("[boot] fase: appInit OK");

    if (nativeResize)
        nativeResize(&jni, NULL, ENGINE_W, ENGINE_H);

    if (nativeResume)
        nativeResume(&jni, NULL);

    l_success("entrando al loop principal...");
    l_error("[boot] fase: loop iniciado");

    {
        /* Prioridad más alta que main (número menor): la Vita no reparte tiempo
         * entre hilos de igual prioridad, y main puede pasar minutos en CPU. */
        SceUID wdog = sceKernelCreateThread("hos_watchdog", watchdog_thread,
                                            0x10000100 - 20, 0x4000, 0, 0, NULL);
        if (wdog >= 0)
            sceKernelStartThread(wdog, 0, NULL);
        else
            l_warn("[watchdog] no se pudo crear (0x%08x)", (unsigned)wdog);
    }
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    int pending_key_down = -1, pending_key_up = -1;

    while (1) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) <= 0)
            sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        /* START+SELECT = salir (como en los ports hermanos). */
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT))
            break;

        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;

        if (pressed & SCE_CTRL_START)
            pending_key_down = AKEYCODE_MENU;
        if (released & SCE_CTRL_START)
            pending_key_up = AKEYCODE_MENU;
        pad_buttons(pressed, released);
        pad_joystick(&pad);
        front_touch(&touch);

        if (pending_key_down != -1 && nativeKeyDown) {
            nativeKeyDown(&jni, NULL, pending_key_down);
            pending_key_down = -1;
        }
        if (pending_key_up != -1 && nativeKeyUp) {
            nativeKeyUp(&jni, NULL, pending_key_up);
            pending_key_up = -1;
        }

        if (nativeRender) {
            g_render_entered++;
            nativeRender(&jni, NULL);
            g_render_done++;
        }

        /* onDrawFrame también llamaba a GLMediaPlayer.updateSound(). */
        audio_update_sound();

        gl_draw_splash();
        gl_heartbeat();
        gl_swap();
    }

    /* Salida limpia: drenar como hace onPause/onDestroy en Java. */
    if (nativePause && nativeCanInterrupt) {
        while (nativeCanInterrupt(&jni, NULL) == 0)
            sceKernelDelayThread(10 * 1000);
        nativePause(&jni, NULL);
    }
    if (nativeDone)
        nativeDone(&jni, NULL);
    audio_shutdown();

    sceKernelExitDeleteThread(0);
    return 0;
}
