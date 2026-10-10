/*
 * java.c -- Lado "Java" para FalsoJNI (Hero of Sparta).
 *
 * El .so resuelve sus callbacks con GetMethodID sobre estas clases
 * (ver Java_*_nativeInit en out_ghidra.c):
 *   GLMediaPlayer: 22 métodos (audio real en source/audio.c, video = skip)
 *   GLResLoader:    getResourceFull / getResourceBytes / getResourceLength
 *   HeroOfSparta:   Exit/Pause/config/billing (no-ops documentados)
 * GLUtils.Device no tiene ningún native en el .so (el "nativeInit" que lista
 * el plan era heurística del toolkit) y no se registra.
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "audio.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static void hos_tolower(char *s);
static const char *hos_strip(const char *name);
static int hos_resolve(const char *name, char *out, size_t out_size);

/* ------------------------------------------------------------------ */
/* GLResLoader: resolución de assets                                   */
/*                                                                     */
/* Java (GLResLoader.java) intenta, en orden:                          */
/*  1. drawable res_<nombre-base-en-minúsculas> vía Resources,          */
/*  2. assets.open(ruta) vía AssetManager.                             */
/* En Vita se replica contra DATA_PATH:                                */
/*  1. res/drawable y res/drawable-hdpi-v4, archivo res_<base> (cualq. ext) */
/*  2. assets/<ruta>                                                     */
/*  3. hos/<ruta> y hos/<basename> (pack descargado)                      */
/*  4. la ruta tal cual (ya traducida si era /mnt/sdcard/...).           */
/* ------------------------------------------------------------------ */

static void hos_tolower(char *s) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_tolower() java.c:%d", __LINE__); }
#endif
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* Quita prefijos ".//" / "./" y espacios, como hace Java. */
static const char *hos_strip(const char *name) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_strip() java.c:%d", __LINE__); }
#endif
    static __thread char buf[512];
    const char *p = name ? name : "";
    if (strncmp(p, ".//", 3) == 0)
        p += 3;
    else if (strncmp(p, "./", 2) == 0)
        p += 2;
    while (*p == ' ')
        p++;
    snprintf(buf, sizeof(buf), "%s", p);
    /* trim trailing */
    size_t n = strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') {
        buf[n - 1] = '\0';
        n--;
    }
    return buf;
}

/* Traduce rutas absolutas Android a DATA_PATH (igual que io.c). */
static const char *hos_translate(const char *name) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_translate() java.c:%d", __LINE__); }
#endif
    static __thread char buf[512];
    const char *sd1 = "/mnt/sdcard/gameloft/games/hos/";
    const char *sd2 = "/sdcard/gameloft/games/hos/";
    if (!name)
        return "";
    if (strncmp(name, sd1, strlen(sd1)) == 0) {
        snprintf(buf, sizeof(buf), DATA_PATH "hos/%s", name + strlen(sd1));
        return buf;
    }
    if (strncmp(name, sd2, strlen(sd2)) == 0) {
        snprintf(buf, sizeof(buf), DATA_PATH "hos/%s", name + strlen(sd2));
        return buf;
    }
    return name;
}

static int hos_try(const char *path, char *out, size_t out_size) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_try() java.c:%d", __LINE__); }
#endif
    struct stat st;
    if (!path || !*path)
        return 0;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        snprintf(out, out_size, "%s", path);
        return 1;
    }
    return 0;
}

/* Busca res_<base>.* en res/drawable* (paso 1 de Java). */
static int hos_find_drawable(const char *stripped, char *out, size_t out_size) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_find_drawable() java.c:%d", __LINE__); }
#endif
    static __thread char base[256];
    static __thread char want[256];
    const char *slash = strrchr(stripped, '/');
    const char *file = slash ? slash + 1 : stripped;
    const char *dot = strrchr(file, '.');
    size_t len = dot ? (size_t)(dot - file) : strlen(file);
    static const char *dirs[] = {
        DATA_PATH "res/drawable/",
        DATA_PATH "res/drawable-hdpi-v4/",
        NULL,
    };

    if (len == 0 || len >= sizeof(base))
        return 0;
    memcpy(base, file, len);
    base[len] = '\0';
    hos_tolower(base);
    snprintf(want, sizeof(want), "res_%s", base);

    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        struct dirent *de;
        if (!dp)
            continue;
        while ((de = readdir(dp)) != NULL) {
            static __thread char low[256];
            static __thread char cand[512];
            snprintf(low, sizeof(low), "%s", de->d_name);
            hos_tolower(low);
            if (strncmp(low, want, strlen(want)) == 0) {
                closedir(dp);
                snprintf(cand, sizeof(cand), "%s%s", dirs[d], de->d_name);
                if (hos_try(cand, out, out_size))
                    return 1;
                return 0;
            }
        }
        closedir(dp);
    }
    return 0;
}

static int hos_resolve(const char *name, char *out, size_t out_size) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> hos_resolve() java.c:%d", __LINE__); }
#endif
    static __thread char cand[512];
    const char *tr = hos_translate(name);
    const char *st = hos_strip(tr);

    /* 1. drawable */
    if (hos_find_drawable(st, out, out_size))
        return 1;
    /* 2. assets/ */
    snprintf(cand, sizeof(cand), DATA_PATH "assets/%s", st);
    if (hos_try(cand, out, out_size))
        return 1;
    /* 3. hos/ (pack) */
    snprintf(cand, sizeof(cand), DATA_PATH "hos/%s", st);
    if (hos_try(cand, out, out_size))
        return 1;
    {
        const char *slash = strrchr(st, '/');
        if (slash) {
            snprintf(cand, sizeof(cand), DATA_PATH "hos/%s", slash + 1);
            if (hos_try(cand, out, out_size))
                return 1;
        }
    }
    /* 4. tal cual */
    if (hos_try(tr, out, out_size))
        return 1;
    return 0;
}

jobject GLResLoader_getResourceFull(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLResLoader_getResourceFull() java.c:%d", __LINE__); }
#endif
    const char *name = (const char *)va_arg(args, jstring);
    char path[512];
    FILE *f;
    struct stat st;
    JavaDynArray *jda;
    (void)id;

    /* Marcador una-sola-vez: si el motor llega a JNI, esta línea aparece
     * aunque todo lo demás falle después (bug #1). */
    {
        static int seen = 0;
        if (!seen) {
            seen = 1;
            l_error("[Java] primera llamada JNI->ResLoader (getResourceFull): %s",
                    name ? name : "(null)");
        }
    }

    if (!hos_resolve(name, path, sizeof(path))) {
        l_warn("[Java] GLResLoader.getResourceFull(%s): no encontrado",
               name ? name : "(null)");
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    jda = jda_alloc((jsize)st.st_size, FIELD_TYPE_BYTE);
    if (!jda) {
        fclose(f);
        return NULL;
    }
    if (fread(jda->array, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
        l_warn("[Java] getResourceFull(%s): lectura corta", name);
    fclose(f);
    l_debug("[Java] GLResLoader.getResourceFull(%s) -> %s (%ld bytes)", name,
            path, (long)st.st_size);
    return (jobject)jda;
}

jint GLResLoader_getResourceLength(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLResLoader_getResourceLength() java.c:%d", __LINE__); }
#endif
    const char *name = (const char *)va_arg(args, jstring);
    char path[512];
    struct stat st;
    (void)id;

    if (!hos_resolve(name, path, sizeof(path))) {
        l_warn("[Java] GLResLoader.getResourceLength(%s): no encontrado",
               name ? name : "(null)");
        return 0;
    }
    if (stat(path, &st) != 0)
        return 0;
    return (jint)st.st_size;
}

jobject GLResLoader_getResourceBytes(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLResLoader_getResourceBytes() java.c:%d", __LINE__); }
#endif
    const char *name = (const char *)va_arg(args, jstring);
    jint offset = va_arg(args, jint);
    jint length = va_arg(args, jint);
    char path[512];
    FILE *f;
    JavaDynArray *jda;
    (void)id;

    if (length <= 0 || !hos_resolve(name, path, sizeof(path))) {
        l_warn("[Java] GLResLoader.getResourceBytes(%s,%d,%d): no encontrado",
               name ? name : "(null)", (int)offset, (int)length);
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (offset > 0)
        fseek(f, offset, SEEK_SET);
    jda = jda_alloc(length, FIELD_TYPE_BYTE);
    if (!jda) {
        fclose(f);
        return NULL;
    }
    fread(jda->array, 1, (size_t)length, f);
    fclose(f);
    return (jobject)jda;
}

/* ------------------------------------------------------------------ */
/* HeroOfSparta (valores leídos de HeroOfSparta.java)                   */
/* ------------------------------------------------------------------ */

void HeroOfSparta_Exit(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_Exit() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    l_info("[Java] HeroOfSparta.Exit(): el motor pidió salir");
    sceKernelExitProcess(0);
}

void HeroOfSparta_Pause(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_Pause() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    l_info("[Java] HeroOfSparta.Pause() (no-op: la pausa la maneja el loop)");
}

void HeroOfSparta_openBrowser(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_openBrowser() java.c:%d", __LINE__); }
#endif
    const char *url = (const char *)va_arg(args, jobject);
    (void)id;
    l_info("[Java] HeroOfSparta.openBrowser(%s) (no-op: sin navegador en Vita)",
           url ? url : "(null)");
}

void HeroOfSparta_OpenIGP(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_OpenIGP() java.c:%d", __LINE__); }
#endif
    (void)va_arg(args, jint);
    (void)id;
    l_info("[Java] HeroOfSparta.OpenIGP() (no-op: sin cross-promo en Vita)");
}

void HeroOfSparta_LaunchBilling(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_LaunchBilling() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    l_info("[Java] HeroOfSparta.LaunchBilling() (no-op: sin facturación en Vita)");
}

void HeroOfSparta_TrackingRegisterFirstRun(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_TrackingRegisterFirstRun() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
}

void HeroOfSparta_CreateNomedia(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_CreateNomedia() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    /* .nomedia ya va en el pack (hos/.nomedia). */
}

void HeroOfSparta_loadConfig(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_loadConfig() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    l_debug("[Java] HeroOfSparta.loadConfig() (no-op v1)");
}

void HeroOfSparta_updateNumLaunch(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_updateNumLaunch() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
}

void HeroOfSparta_updateModeGame(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_updateModeGame() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
}

void HeroOfSparta_setStateCompleteDemo(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_setStateCompleteDemo() java.c:%d", __LINE__); }
#endif
    (void)va_arg(args, int);
    (void)id;
}

void HeroOfSparta_unlockDemo(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_unlockDemo() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
}

jint HeroOfSparta_IsDemo(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_IsDemo() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return 0; /* Java lo hardcodea a 0 en este build (juego completo). */
}

jint HeroOfSparta_IsDoubleOption(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_IsDoubleOption() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return 0;
}

jint HeroOfSparta_detectPhoneLang(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_detectPhoneLang() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return 0; /* 0 = inglés (ver HeroOfSparta.detectPhoneLang). */
}

jint HeroOfSparta_getNumLaunch(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_getNumLaunch() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return 0;
}

jint HeroOfSparta_getVersionMode(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_getVersionMode() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return 1; /* default de SharedPreferences "playMode". */
}

jboolean HeroOfSparta_isCompleteDemo(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_isCompleteDemo() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return JNI_FALSE;
}

jobject HeroOfSparta_GetDoubleOptionText(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> HeroOfSparta_GetDoubleOptionText() java.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return NULL; /* ServerInfo h es null sin red; Java haría NPE igual. */
}

/* ------------------------------------------------------------------ */
/* Tablas FalsoJNI                                                     */
/* ------------------------------------------------------------------ */

enum {
    M_getResourceFull = 1,
    M_getResourceBytes,
    M_getResourceLength,

    M_isSoundLoaded,
    M_getSoundDuration,
    M_getSoundStatus,
    M_isMediaPlaying,
    M_isMusicPlaying,
    M_loadSound,
    M_playSound,
    M_pauseSound,
    M_resumeSound,
    M_stopSound,
    M_setSoundVolume,
    M_resetSound,
    M_stopAllSounds,
    M_unloadSound,
    M_updateSound,
    M_loadMusic,
    M_playMusic,
    M_pauseMusic,
    M_resumeMusic,
    M_stopMusic,
    M_setMusicVolume,
    M_unloadMusic,
    M_stopAllMusic,
    M_IsVedioFinished,
    M_ResumeMovie,
    M_loadMovie,
    M_loadMovieL,

    M_Exit,
    M_Pause,
    M_openBrowser,
    M_OpenIGP,
    M_LaunchBilling,
    M_TrackingRegisterFirstRun,
    M_CreateNomedia,
    M_loadConfig,
    M_updateNumLaunch,
    M_updateModeGame,
    M_setStateCompleteDemo,
    M_unlockDemo,
    M_IsDemo,
    M_IsDoubleOption,
    M_detectPhoneLang,
    M_getNumLaunch,
    M_getVersionMode,
    M_isCompleteDemo,
    M_GetDoubleOptionText1,
    M_GetDoubleOptionText2,
    M_GetDoubleOptionText3,
};

NameToMethodID nameToMethodId[] = {
    { M_getResourceFull,   "getResourceFull",   METHOD_TYPE_OBJECT },
    { M_getResourceBytes,  "getResourceBytes",  METHOD_TYPE_OBJECT },
    { M_getResourceLength, "getResourceLength", METHOD_TYPE_INT },

    { M_isSoundLoaded,   "isSoundLoaded",   METHOD_TYPE_INT },
    { M_getSoundDuration, "getSoundDuration", METHOD_TYPE_INT },
    { M_getSoundStatus,  "getSoundStatus",  METHOD_TYPE_INT },
    { M_isMediaPlaying,  "isMediaPlaying",  METHOD_TYPE_INT },
    { M_isMusicPlaying,  "isMusicPlaying",  METHOD_TYPE_INT },
    { M_loadSound,       "loadSound",       METHOD_TYPE_VOID },
    { M_playSound,       "playSound",       METHOD_TYPE_VOID },
    { M_pauseSound,      "pauseSound",      METHOD_TYPE_VOID },
    { M_resumeSound,     "resumeSound",     METHOD_TYPE_VOID },
    { M_stopSound,       "stopSound",       METHOD_TYPE_VOID },
    { M_setSoundVolume,  "setSoundVolume",  METHOD_TYPE_VOID },
    { M_resetSound,      "resetSound",      METHOD_TYPE_VOID },
    { M_stopAllSounds,   "stopAllSounds",   METHOD_TYPE_VOID },
    { M_unloadSound,     "unloadSound",     METHOD_TYPE_VOID },
    { M_updateSound,     "updateSound",     METHOD_TYPE_VOID },
    { M_loadMusic,       "loadMusic",       METHOD_TYPE_VOID },
    { M_playMusic,       "playMusic",       METHOD_TYPE_VOID },
    { M_pauseMusic,      "pauseMusic",      METHOD_TYPE_VOID },
    { M_resumeMusic,     "resumeMusic",     METHOD_TYPE_VOID },
    { M_stopMusic,       "stopMusic",       METHOD_TYPE_VOID },
    { M_setMusicVolume,  "setMusicVolume",  METHOD_TYPE_VOID },
    { M_unloadMusic,     "unloadMusic",     METHOD_TYPE_VOID },
    { M_stopAllMusic,    "stopAllMusic",    METHOD_TYPE_VOID },
    { M_IsVedioFinished, "IsVedioFinished", METHOD_TYPE_INT },
    { M_ResumeMovie,     "ResumeMovie",     METHOD_TYPE_VOID },
    { M_loadMovie,       "loadMovie",       METHOD_TYPE_VOID },
    { M_loadMovieL,      "loadMovieL",      METHOD_TYPE_VOID },

    { M_Exit,                     "Exit",                     METHOD_TYPE_VOID },
    { M_Pause,                    "Pause",                    METHOD_TYPE_VOID },
    { M_openBrowser,              "openBrowser",              METHOD_TYPE_VOID },
    { M_OpenIGP,                  "OpenIGP",                  METHOD_TYPE_VOID },
    { M_LaunchBilling,            "LaunchBilling",            METHOD_TYPE_VOID },
    { M_TrackingRegisterFirstRun, "TrackingRegisterFirstRun", METHOD_TYPE_VOID },
    { M_CreateNomedia,            "CreateNomedia",            METHOD_TYPE_VOID },
    { M_loadConfig,               "loadConfig",               METHOD_TYPE_VOID },
    { M_updateNumLaunch,          "updateNumLaunch",          METHOD_TYPE_VOID },
    { M_updateModeGame,           "updateModeGame",           METHOD_TYPE_VOID },
    { M_setStateCompleteDemo,     "setStateCompleteDemo",     METHOD_TYPE_VOID },
    { M_unlockDemo,               "unlockDemo",               METHOD_TYPE_VOID },
    { M_IsDemo,                   "IsDemo",                   METHOD_TYPE_INT },
    { M_IsDoubleOption,           "IsDoubleOption",           METHOD_TYPE_INT },
    { M_detectPhoneLang,          "detectPhoneLang",          METHOD_TYPE_INT },
    { M_getNumLaunch,             "getNumLaunch",             METHOD_TYPE_INT },
    { M_getVersionMode,           "getVersionMode",           METHOD_TYPE_INT },
    { M_isCompleteDemo,           "isCompleteDemo",           METHOD_TYPE_BOOLEAN },
    { M_GetDoubleOptionText1,     "GetDoubleOptionText1",     METHOD_TYPE_OBJECT },
    { M_GetDoubleOptionText2,     "GetDoubleOptionText2",     METHOD_TYPE_OBJECT },
    { M_GetDoubleOptionText3,     "GetDoubleOptionText3",     METHOD_TYPE_OBJECT },
};

MethodsBoolean methodsBoolean[] = {
    { M_isCompleteDemo, HeroOfSparta_isCompleteDemo },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

MethodsInt methodsInt[] = {
    { M_getResourceLength, GLResLoader_getResourceLength },
    { M_isSoundLoaded,     GLMediaPlayer_isSoundLoaded },
    { M_getSoundDuration,  GLMediaPlayer_getSoundDuration },
    { M_getSoundStatus,    GLMediaPlayer_getSoundStatus },
    { M_isMediaPlaying,    GLMediaPlayer_isMediaPlaying },
    { M_isMusicPlaying,    GLMediaPlayer_isMusicPlaying },
    { M_IsVedioFinished,   GLMediaPlayer_IsVedioFinished },
    { M_IsDemo,            HeroOfSparta_IsDemo },
    { M_IsDoubleOption,    HeroOfSparta_IsDoubleOption },
    { M_detectPhoneLang,   HeroOfSparta_detectPhoneLang },
    { M_getNumLaunch,      HeroOfSparta_getNumLaunch },
    { M_getVersionMode,    HeroOfSparta_getVersionMode },
};

MethodsLong methodsLong[] = {};

MethodsObject methodsObject[] = {
    { M_getResourceFull,      GLResLoader_getResourceFull },
    { M_getResourceBytes,     GLResLoader_getResourceBytes },
    { M_GetDoubleOptionText1, HeroOfSparta_GetDoubleOptionText },
    { M_GetDoubleOptionText2, HeroOfSparta_GetDoubleOptionText },
    { M_GetDoubleOptionText3, HeroOfSparta_GetDoubleOptionText },
};

MethodsShort methodsShort[] = {};

MethodsVoid methodsVoid[] = {
    { M_loadSound,       GLMediaPlayer_loadSound },
    { M_playSound,       GLMediaPlayer_playSound },
    { M_pauseSound,      GLMediaPlayer_pauseSound },
    { M_resumeSound,     GLMediaPlayer_resumeSound },
    { M_stopSound,       GLMediaPlayer_stopSound },
    { M_setSoundVolume,  GLMediaPlayer_setSoundVolume },
    { M_resetSound,      GLMediaPlayer_resetSound },
    { M_stopAllSounds,   GLMediaPlayer_stopAllSounds },
    { M_unloadSound,     GLMediaPlayer_unloadSound },
    { M_updateSound,     GLMediaPlayer_updateSound },
    { M_loadMusic,       GLMediaPlayer_loadMusic },
    { M_playMusic,       GLMediaPlayer_playMusic },
    { M_pauseMusic,      GLMediaPlayer_pauseMusic },
    { M_resumeMusic,     GLMediaPlayer_resumeMusic },
    { M_stopMusic,       GLMediaPlayer_stopMusic },
    { M_setMusicVolume,  GLMediaPlayer_setMusicVolume },
    { M_unloadMusic,     GLMediaPlayer_unloadMusic },
    { M_stopAllMusic,    GLMediaPlayer_stopAllMusic },
    { M_ResumeMovie,     GLMediaPlayer_ResumeMovie },
    { M_loadMovie,       GLMediaPlayer_loadMovie },
    { M_loadMovieL,      GLMediaPlayer_loadMovieL },
    { M_Exit,                     HeroOfSparta_Exit },
    { M_Pause,                    HeroOfSparta_Pause },
    { M_openBrowser,              HeroOfSparta_openBrowser },
    { M_OpenIGP,                  HeroOfSparta_OpenIGP },
    { M_LaunchBilling,            HeroOfSparta_LaunchBilling },
    { M_TrackingRegisterFirstRun, HeroOfSparta_TrackingRegisterFirstRun },
    { M_CreateNomedia,            HeroOfSparta_CreateNomedia },
    { M_loadConfig,               HeroOfSparta_loadConfig },
    { M_updateNumLaunch,          HeroOfSparta_updateNumLaunch },
    { M_updateModeGame,           HeroOfSparta_updateModeGame },
    { M_setStateCompleteDemo,     HeroOfSparta_setStateCompleteDemo },
    { M_unlockDemo,               HeroOfSparta_unlockDemo },
};

/*
 * Campos JNI.
 */
char WINDOW_SERVICE[] = "window";

const int SDK_INT = 19; /* Android 4.4 / KitKat */

NameToFieldID nameToFieldId[] = {
    { 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
    { 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    { 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
    { 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
