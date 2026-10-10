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

/* IDs de puntero para los toques sintéticos (el motor tiene 10 slots que asigna
 * por ID, Main::getTouchIdx); 0 queda para el dedo real. */
#define TOUCH_ID_JOY    5
#define TOUCH_ID_SWORD  6
#define TOUCH_ID_SHIELD 7

/* Botones del HUD en el espacio 1280x752 del motor (medidos sobre captura). */
#define SWORD_X  1127
#define SWORD_Y  516
#define SHIELD_X 1016
#define SHIELD_Y 639
/* Desplazamiento del joystick: debe quedar dentro del radio de 100 px con que
 * Main::ProcessJoystick acepta el toque. */
#define JOY_RADIUS 60
#define STICK_DEADZONE 32

#define TOUCH_ID_KILL   8
/* Zona táctil del botón de focused kill (ShowFatalityButton muestra 12 y 20). */
#define KILL_AREA_ITEM  12

static uintptr_t *p_canvas; /* &m_canvas del .so (Main*) */

int hud_area_center(uintptr_t gui, int item, int *x, int *y); /* patch.c */
static void touch(int id, int type, int x, int y);

/* TRIANGLE -> toque sobre el centro real de la zona del botón de focused kill. */
static void pad_kill(unsigned pressed, unsigned released) {
    static int kx, ky, down;
    uintptr_t gui;
    if ((pressed & SCE_CTRL_TRIANGLE) && p_canvas && *p_canvas &&
        (gui = *(uintptr_t *)(*p_canvas + 0x374)) &&
        hud_area_center(gui, KILL_AREA_ITEM, &kx, &ky)) {
        touch(TOUCH_ID_KILL, 1, kx, ky);
        down = 1;
    }
    if ((released & SCE_CTRL_TRIANGLE) && down) {
        touch(TOUCH_ID_KILL, 0, kx, ky);
        down = 0;
    }
}

static void touch(int id, int type, int x, int y) {
    if (nativeOnTouch)
        nativeOnTouch(&jni, NULL, id, type, x, y);
}

/* Botón físico -> toque mantenido sobre un botón del HUD. */
static void pad_button(unsigned pressed, unsigned released, unsigned btn, int id, int x, int y) {
    if (pressed & btn)
        touch(id, 1, x, y);
    if (released & btn)
        touch(id, 0, x, y);
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
    int finger_down = 0;
    int last_tx = 0, last_ty = 0;
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
        pad_button(pressed, released, SCE_CTRL_CROSS, TOUCH_ID_SWORD, SWORD_X, SWORD_Y);
        pad_button(pressed, released, SCE_CTRL_CIRCLE, TOUCH_ID_SHIELD, SHIELD_X, SHIELD_Y);
        pad_joystick(&pad);
        pad_kill(pressed, released);

        /* Táctil frontal -> nativeOnTouch(id, tipo, x, y).
         * Tipos según HeroOfSparta.onTouchEvent: 1 = down, 2 = move, 0 = up.
         * Siempre slot 0: el motor indexa slots sin validar rango. */
        if (touch.reportNum > 0) {
            /* El motor lee el táctil en su espacio 1280x752 (ver glutil.h). */
            int phys_x = (int)touch.report[0].x * ENGINE_W / 1920;
            int phys_y = (int)touch.report[0].y * ENGINE_H / 1088;
            if (!finger_down) {
                if (nativeOnTouch)
                    nativeOnTouch(&jni, NULL, 0, 1, phys_x, phys_y);
                l_debug("touch DOWN (%d,%d)", phys_x, phys_y);
                finger_down = 1;
            } else if (phys_x != last_tx || phys_y != last_ty) {
                if (nativeOnTouch)
                    nativeOnTouch(&jni, NULL, 0, 2, phys_x, phys_y);
            }
            last_tx = phys_x;
            last_ty = phys_y;
        } else if (finger_down) {
            if (nativeOnTouch)
                nativeOnTouch(&jni, NULL, 0, 0, last_tx, last_ty);
            l_debug("touch UP (%d,%d)", last_tx, last_ty);
            finger_down = 0;
        }

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
