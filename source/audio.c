/*
 * audio.c -- GLMediaPlayer sobre SceAudioOut (ver audio.h).
 *
 * Sonidos 0..95 y músicas 0..15 decodificados de hos/<id>.ogg /
 * hos/m_<id>.ogg con stb_vorbis bajo demanda. Un hilo mezcla todas las
 * voces activas a 44100 Hz stereo S16 y lo entrega a un puerto BGM.
 */

#include "audio.h"
#include "utils/logger.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_vorbis/stb_vorbis.c"

#define HOS_NUM_SOUNDS 96
#define HOS_NUM_MUSICS 16

#define MIX_RATE 44100
#define MIX_GRAIN 1024

/* Estados (== GLMediaPlayer.java). */
#define ST_EMPTY 0
#define ST_UNLOADED 1
#define ST_LOADED 5
#define ST_PLAYING 6
#define ST_STOPPED 7
#define ST_PAUSED 8

typedef struct {
    short *pcm;          /* interleaved, ch canales, rate Hz */
    int frames;
    int channels;
    int rate;
    volatile int status;
    volatile float pos;  /* posición actual en frames (float por resample) */
    volatile int loop;
    float vol;           /* volumen por voz (músicas comparten g_music_vol) */
    stb_vorbis *vf;      /* música: decodificación en streaming desde ogg */
    unsigned char *ogg;  /* .ogg completo en RAM mientras vf esté abierto */
} voice_t;

#define HAS_DATA(v) ((v).pcm || (v).vf)

static voice_t g_sounds[HOS_NUM_SOUNDS];
static voice_t g_musics[HOS_NUM_MUSICS];
static volatile float g_sound_vol = 1.0f;
static volatile float g_music_vol = 1.0f;

static SceUID g_mix_thread = -1;
static volatile int g_mix_quit = 0;
static int g_audio_port = -1;
static SceKernelLwMutexWork g_audio_mutex;

static void audio_lock(void) { sceKernelLockLwMutex(&g_audio_mutex, 1, NULL); }
static void audio_unlock(void) { sceKernelUnlockLwMutex(&g_audio_mutex, 1); }

/* Lee un archivo completo a memoria. 0 = ok. */
static int read_file(const char *path, unsigned char **out, int *out_len) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> read_file() audio.c:%d", __LINE__); }
#endif
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *buf;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_len = (int)sz;
    return 0;
}

static void voice_free(voice_t *v) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> voice_free() audio.c:%d", __LINE__); }
#endif
    if (v->pcm) {
        free(v->pcm);
        v->pcm = NULL;
    }
    if (v->vf) {
        stb_vorbis_close(v->vf);
        v->vf = NULL;
    }
    free(v->ogg);
    v->ogg = NULL;
    v->frames = 0;
    v->pos = 0;
    v->loop = 0;
}

/* Decodifica (o abre en streaming) un .ogg y lo instala en la voz. Llamar SIN
 * el mutex: la decodificación tarda (hasta segundos) y no debe frenar al
 * mezclador; solo el intercambio final va bajo lock. */
static void voice_load(voice_t *v, const char *path, int stream) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> voice_load() audio.c:%d", __LINE__); }
#endif
    unsigned char *data = NULL;
    int data_len = 0;
    short *out = NULL;
    int channels = 0, rate = 0, frames = 0;

    stb_vorbis *vf = NULL;

    if (read_file(path, &data, &data_len) != 0) {
        l_warn("[audio] falta archivo: %s", path);
        v->status = ST_UNLOADED;
        return;
    }
    if (stream) {
        vf = stb_vorbis_open_memory(data, data_len, NULL, NULL);
        if (vf) {
            stb_vorbis_info info = stb_vorbis_get_info(vf);
            channels = info.channels;
            rate = info.sample_rate;
            frames = (int)stb_vorbis_stream_length_in_samples(vf);
            /* El streaming mezcla sin resample: solo si ya viene a MIX_RATE. */
            if (rate != MIX_RATE) {
                stb_vorbis_close(vf);
                vf = NULL;
            }
        }
    }
    if (!vf) {
        frames = stb_vorbis_decode_memory(data, data_len, &channels, &rate, &out);
        free(data);
        data = NULL;
        if (frames <= 0 || !out) {
            l_warn("[audio] no se pudo decodificar: %s", path);
            v->status = ST_UNLOADED;
            return;
        }
    }
    audio_lock();
    voice_free(v);
    v->pcm = out;
    v->vf = vf;
    v->ogg = data;
    v->frames = frames;
    v->channels = channels;
    v->rate = rate;
    v->pos = 0;
    v->loop = 0;
    v->vol = 1.0f;
    v->status = ST_LOADED;
    audio_unlock();
    l_debug("[audio] cargado %s (%d frames, %dch, %dHz%s)", path, frames, channels, rate,
            vf ? ", streaming" : "");
}

static void voice_play(voice_t *v, int loop) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> voice_play() audio.c:%d", __LINE__); }
#endif
    if (!HAS_DATA(*v) || v->status == ST_EMPTY || v->status == ST_UNLOADED)
        return;
    if (v->status == ST_PLAYING) {
        v->pos = 0; /* Java: seekTo(0) si ya sonaba */
        if (v->vf)
            stb_vorbis_seek_start(v->vf);
        return;
    }
    v->loop = loop;
    if (v->status != ST_PAUSED) {
        v->pos = 0;
        if (v->vf)
            stb_vorbis_seek_start(v->vf);
    }
    v->status = ST_PLAYING;
}

static int mix_thread(SceSize args, void *argp) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> mix_thread() audio.c:%d", __LINE__); }
#endif
    static short mixbuf[MIX_GRAIN * 2];
    (void)args;
    (void)argp;

    while (!g_mix_quit) {
        sceClibMemset(mixbuf, 0, sizeof(mixbuf));
        audio_lock();
        for (int vi = 0; vi < 2; vi++) {
            voice_t *bank = (vi == 0) ? g_sounds : g_musics;
            int n = (vi == 0) ? HOS_NUM_SOUNDS : HOS_NUM_MUSICS;
            float gvol = (vi == 0) ? g_sound_vol : g_music_vol;
            for (int i = 0; i < n; i++) {
                voice_t *v = &bank[i];
                float step;
                if (v->status == ST_PLAYING && v->vf) {
                    static short tmp[MIX_GRAIN * 2];
                    int got = 0, empty = 0;
                    while (got < MIX_GRAIN && empty < 2) {
                        int n = stb_vorbis_get_samples_short_interleaved(
                            v->vf, 2, tmp + got * 2, (MIX_GRAIN - got) * 2);
                        if (n > 0) {
                            got += n;
                            empty = 0;
                        } else if (v->loop) {
                            stb_vorbis_seek_start(v->vf);
                            empty++; /* 2 vacíos seguidos = ogg roto, cortar */
                        } else {
                            v->status = ST_STOPPED;
                            break;
                        }
                    }
                    for (int s = 0; s < got * 2; s++)
                        mixbuf[s] += (short)(tmp[s] * gvol * v->vol);
                    continue;
                }
                if (v->status != ST_PLAYING || !v->pcm || v->frames <= 0)
                    continue;
                if (v->rate <= 0)
                    continue;
                step = (float)v->rate / (float)MIX_RATE;
                for (int s = 0; s < MIX_GRAIN; s++) {
                    int p0 = (int)v->pos;
                    float l, r;
                    if (p0 >= v->frames) {
                        if (v->loop) {
                            v->pos = 0;
                            p0 = 0;
                        } else {
                            v->status = ST_STOPPED;
                            break;
                        }
                    }
                    if (v->channels == 2) {
                        l = (float)v->pcm[p0 * 2];
                        r = (float)v->pcm[p0 * 2 + 1];
                    } else {
                        l = r = (float)v->pcm[p0];
                    }
                    mixbuf[s * 2] += (short)(l * gvol * v->vol);
                    mixbuf[s * 2 + 1] += (short)(r * gvol * v->vol);
                    v->pos += step;
                }
            }
        }
        audio_unlock();
        sceAudioOutOutput(g_audio_port, mixbuf);
    }
    return 0;
}

/* Decodifica todos los SFX una vez al arrancar (1.6 MB de ogg, ~20 MB PCM): así
 * ningún loadSound/playSound decodifica en pleno juego. */
static int sfx_preload_thread(SceSize args, void *argp) {
    char path[256];
    (void)args;
    (void)argp;
    for (int i = 0; i < HOS_NUM_SOUNDS && !g_mix_quit; i++) {
        if (i == 60 || i == 66 || i == 95 || g_sounds[i].pcm)
            continue;
        snprintf(path, sizeof(path), DATA_PATH "hos/%d.ogg", i);
        voice_load(&g_sounds[i], path, 0);
        audio_lock();
        if (g_sounds[i].status == ST_LOADED)
            g_sounds[i].status = ST_UNLOADED; /* cacheado, pero el motor aún no lo pidió */
        audio_unlock();
    }
    l_info("[audio] SFX precargados");
    sceKernelExitDeleteThread(0);
    return 0;
}

void audio_init(void) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> audio_init() audio.c:%d", __LINE__); }
#endif
    int ret;

    sceClibMemset(g_sounds, 0, sizeof(g_sounds));
    sceClibMemset(g_musics, 0, sizeof(g_musics));
    for (int i = 0; i < HOS_NUM_SOUNDS; i++)
        g_sounds[i].status = ST_EMPTY;
    for (int i = 0; i < HOS_NUM_MUSICS; i++)
        g_musics[i].status = ST_EMPTY;

    ret = sceKernelCreateLwMutex(&g_audio_mutex, "hos_audio", 0, 0, NULL);
    if (ret < 0) {
        l_error("[audio] no se pudo crear el mutex (0x%08x), audio apagado", ret);
        return;
    }

    g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, MIX_GRAIN,
                                       MIX_RATE, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (g_audio_port < 0) {
        l_error("[audio] sceAudioOutOpenPort falló (0x%08x), audio apagado",
                g_audio_port);
        g_audio_port = -1;
        return;
    }

    {
        int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(g_audio_port,
                             SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                             vols);
    }

    /* Prioridad sobre main (la música se decodifica acá en streaming) y 64 KB
     * de stack: stb_vorbis usa alloca para sus buffers temporales. */
    g_mix_thread = sceKernelCreateThread("hos_mixer", mix_thread, 0x10000100 - 10,
                                         0x10000, 0, 0, NULL);
    if (g_mix_thread < 0) {
        l_error("[audio] no se pudo crear el hilo mezclador, audio apagado");
        sceAudioOutReleasePort(g_audio_port);
        g_audio_port = -1;
        return;
    }
    sceKernelStartThread(g_mix_thread, 0, NULL);
    {
        /* Prioridad bajo main: usa la CPU que sobra durante la carga inicial. */
        SceUID pre = sceKernelCreateThread("hos_sfx_preload", sfx_preload_thread,
                                           0x10000100 + 10, 0x10000, 0, 0, NULL);
        if (pre >= 0)
            sceKernelStartThread(pre, 0, NULL);
    }
    l_success("[audio] mezclador iniciado (44100Hz stereo, %d sonidos + %d músicas)",
              HOS_NUM_SOUNDS, HOS_NUM_MUSICS);
}

void audio_shutdown(void) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> audio_shutdown() audio.c:%d", __LINE__); }
#endif
    if (g_mix_thread < 0)
        return;
    g_mix_quit = 1;
    sceKernelWaitThreadEnd(g_mix_thread, NULL, NULL);
    sceKernelDeleteThread(g_mix_thread);
    g_mix_thread = -1;
    if (g_audio_port >= 0) {
        sceAudioOutReleasePort(g_audio_port);
        g_audio_port = -1;
    }
}

/* ---- Puente de sonidos ---- */

static int valid_sound(int i) { return i >= 0 && i < HOS_NUM_SOUNDS; }
static int valid_music(int i) { return i >= 0 && i < HOS_NUM_MUSICS; }

jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_isSoundLoaded() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    int st;
    (void)id;
    if (!valid_sound(i))
        return 0;
    st = g_sounds[i].status;
    return (st != ST_EMPTY && st != ST_UNLOADED && st != 4 && st != ST_STOPPED && st != 2) ? 1 : 0;
}

jint GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_isMediaPlaying() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i) || !g_sounds[i].pcm)
        return 0;
    return g_sounds[i].status == ST_PLAYING ? 1 : 0;
}

jint GLMediaPlayer_getSoundStatus(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_getSoundStatus() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i))
        return 0;
    return g_sounds[i].status;
}

jint GLMediaPlayer_getSoundDuration(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_getSoundDuration() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i) || !g_sounds[i].pcm || g_sounds[i].rate <= 0)
        return 0;
    return (jint)((int64_t)g_sounds[i].frames * 1000 / g_sounds[i].rate);
}

void GLMediaPlayer_loadSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_loadSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    char path[256];
    (void)id;
    /* Java omite 60/66/95 ("sound N.ogg not found!"). */
    if (!valid_sound(i) || i == 60 || i == 66 || i == 95)
        return;
    if (!g_sounds[i].pcm) {
        snprintf(path, sizeof(path), DATA_PATH "hos/%d.ogg", (int)i);
        voice_load(&g_sounds[i], path, 0);
    }
    audio_lock();
    if (g_sounds[i].pcm && g_sounds[i].status == ST_UNLOADED)
        g_sounds[i].status = ST_LOADED; /* PCM cacheado: recarga instantánea */
    audio_unlock();
}

void GLMediaPlayer_playSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_playSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    jint loop = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i) || g_sound_vol <= 0.0f || i == 60 || i == 66 || i == 95)
        return;
    if (!g_sounds[i].pcm) {
        char path[256];
        snprintf(path, sizeof(path), DATA_PATH "hos/%d.ogg", (int)i);
        voice_load(&g_sounds[i], path, 0);
    }
    audio_lock();
    if (g_sounds[i].pcm && g_sounds[i].status == ST_UNLOADED)
        g_sounds[i].status = ST_LOADED;
    voice_play(&g_sounds[i], loop != 0);
    audio_unlock();
}

void GLMediaPlayer_pauseSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_pauseSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    audio_lock();
    if (valid_sound(i)) {
        if (i >= 0) {
            if (g_sounds[i].status == ST_PLAYING)
                g_sounds[i].status = ST_PAUSED;
        } else {
            for (int k = 0; k < HOS_NUM_SOUNDS; k++)
                if (g_sounds[k].status == ST_PLAYING)
                    g_sounds[k].status = ST_PAUSED;
        }
    } else {
        for (int k = 0; k < HOS_NUM_SOUNDS; k++)
            if (g_sounds[k].status == ST_PLAYING)
                g_sounds[k].status = ST_PAUSED;
    }
    audio_unlock();
}

void GLMediaPlayer_resumeSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_resumeSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i))
        return;
    audio_lock();
    if (g_sounds[i].status == ST_PAUSED)
        g_sounds[i].status = ST_PLAYING;
    audio_unlock();
}

void GLMediaPlayer_stopSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_stopSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i))
        return;
    audio_lock();
    if (g_sounds[i].pcm && g_sounds[i].status != ST_STOPPED) {
        g_sounds[i].status = ST_STOPPED;
        g_sounds[i].pos = 0;
    }
    audio_unlock();
}

void GLMediaPlayer_setSoundVolume(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_setSoundVolume() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    jfloat f = (jfloat)va_arg(args, double);
    (void)id;
    (void)i;
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    g_sound_vol = f;
}

void GLMediaPlayer_resetSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_resetSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_sound(i))
        return;
    audio_lock();
    if (g_sounds[i].pcm &&
        (g_sounds[i].status == ST_LOADED || g_sounds[i].status == ST_PLAYING ||
         g_sounds[i].status == ST_PAUSED))
        g_sounds[i].pos = 0;
    audio_unlock();
}

void GLMediaPlayer_unloadSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_unloadSound() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    audio_lock();
    if (i < 0) {
        for (int k = 0; k < HOS_NUM_SOUNDS; k++) {
            voice_free(&g_sounds[k]);
            g_sounds[k].status = ST_UNLOADED;
        }
    } else if (valid_sound(i)) {
        voice_free(&g_sounds[i]);
        g_sounds[i].status = ST_UNLOADED;
    }
    audio_unlock();
}

void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_stopAllSounds() audio.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    audio_lock();
    for (int k = 0; k < HOS_NUM_SOUNDS; k++) {
        if (g_sounds[k].pcm) {
            g_sounds[k].status = ST_STOPPED;
            g_sounds[k].pos = 0;
        }
    }
    for (int k = 0; k < HOS_NUM_MUSICS; k++) {
        if (HAS_DATA(g_musics[k])) {
            g_musics[k].status = ST_STOPPED;
            g_musics[k].pos = 0;
        }
    }
    audio_unlock();
}

void GLMediaPlayer_updateSound(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_updateSound() audio.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    audio_update_sound();
}

void audio_update_sound(void) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> audio_update_sound() audio.c:%d", __LINE__); }
#endif
    /* Java libera sonidos terminados salvo UI (3,4,5,34). */
    audio_lock();
    for (int k = 0; k < HOS_NUM_SOUNDS; k++) {
        if (g_sounds[k].pcm && k != 3 && k != 4 && k != 5 && k != 34 &&
            g_sounds[k].status == ST_STOPPED) {
            /* Java libera el MediaPlayer; acá se conserva el PCM (re-decodificar
             * en pleno juego causaba los bajones) y solo cambia el estado. */
            g_sounds[k].pos = 0;
            g_sounds[k].status = ST_UNLOADED;
        }
    }
    audio_unlock();
}

/* ---- Puente de músicas ---- */

jint GLMediaPlayer_isMusicPlaying(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_isMusicPlaying() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_music(i) || !HAS_DATA(g_musics[i]))
        return 0;
    return g_musics[i].status == ST_PLAYING ? 1 : 0;
}

void GLMediaPlayer_loadMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_loadMusic() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    char path[256];
    (void)id;
    if (!valid_music(i))
        return;
    if (!HAS_DATA(g_musics[i])) {
        snprintf(path, sizeof(path), DATA_PATH "hos/m_%d.ogg", (int)i);
        voice_load(&g_musics[i], path, 1);
    }
}

void GLMediaPlayer_playMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_playMusic() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    jint loop = (jint)va_arg(args, int);
    (void)id;
    (void)loop;
    if (!valid_music(i))
        return;
    if (!HAS_DATA(g_musics[i])) {
        char path[256];
        snprintf(path, sizeof(path), DATA_PATH "hos/m_%d.ogg", (int)i);
        voice_load(&g_musics[i], path, 1);
    }
    audio_lock();
    /* Java siempre pone looping en playMusic. */
    voice_play(&g_musics[i], 1);
    audio_unlock();
}

void GLMediaPlayer_pauseMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_pauseMusic() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    audio_lock();
    for (int k = 0; k < HOS_NUM_MUSICS; k++) {
        if (HAS_DATA(g_musics[k]) && g_musics[k].status == ST_PLAYING)
            g_musics[k].status = ST_PAUSED;
    }
    (void)i;
    audio_unlock();
}

void GLMediaPlayer_resumeMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_resumeMusic() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_music(i))
        return;
    audio_lock();
    if (g_musics[i].status == ST_PAUSED)
        g_musics[i].status = ST_PLAYING;
    audio_unlock();
}

void GLMediaPlayer_stopMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_stopMusic() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    if (!valid_music(i))
        return;
    audio_lock();
    if (HAS_DATA(g_musics[i]) && g_musics[i].status != ST_STOPPED) {
        g_musics[i].status = ST_STOPPED;
        g_musics[i].pos = 0;
    }
    audio_unlock();
}

void GLMediaPlayer_setMusicVolume(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_setMusicVolume() audio.c:%d", __LINE__); }
#endif
    jfloat f = (jfloat)va_arg(args, double);
    (void)id;
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    g_music_vol = f;
}

void GLMediaPlayer_unloadMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_unloadMusic() audio.c:%d", __LINE__); }
#endif
    jint i = (jint)va_arg(args, int);
    (void)id;
    audio_lock();
    if (i < 0) {
        for (int k = 0; k < HOS_NUM_MUSICS; k++) {
            voice_free(&g_musics[k]);
            g_musics[k].status = ST_UNLOADED;
        }
    } else if (valid_music(i)) {
        voice_free(&g_musics[i]);
        g_musics[i].status = ST_UNLOADED;
    }
    audio_unlock();
}

void GLMediaPlayer_stopAllMusic(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_stopAllMusic() audio.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    audio_lock();
    for (int k = 0; k < HOS_NUM_MUSICS; k++) {
        if (HAS_DATA(g_musics[k])) {
            g_musics[k].status = ST_STOPPED;
            g_musics[k].pos = 0;
        }
    }
    audio_unlock();
}

/* ---- Película (sin reproductor en Vita: se salta) ---- */

jint GLMediaPlayer_IsVedioFinished(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_IsVedioFinished() audio.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    return 1;
}

void GLMediaPlayer_ResumeMovie(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_ResumeMovie() audio.c:%d", __LINE__); }
#endif
    (void)id;
    (void)args;
    l_info("[Java] GLMediaPlayer.ResumeMovie() (no-op: sin video en Vita)");
}

void GLMediaPlayer_loadMovie(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_loadMovie() audio.c:%d", __LINE__); }
#endif
    const char *name = (const char *)va_arg(args, jobject);
    (void)id;
    l_info("[Java] GLMediaPlayer.loadMovie(%s) (no-op: sin video en Vita)",
           name ? name : "(null)");
}

void GLMediaPlayer_loadMovieL(jmethodID id, va_list args) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> GLMediaPlayer_loadMovieL() audio.c:%d", __LINE__); }
#endif
    const char *name = (const char *)va_arg(args, jobject);
    (void)va_arg(args, jint);
    (void)id;
    l_info("[Java] GLMediaPlayer.loadMovieL(%s) (no-op: sin video en Vita)",
           name ? name : "(null)");
}
