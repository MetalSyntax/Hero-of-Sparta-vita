/**
 * @file  audio.h
 * @brief Mezclador SceAudioOut que respalda el puente GLMediaPlayer.
 *
 * El .so nunca toca hardware de audio directamente: llama a los métodos
 * estáticos Java de GLMediaPlayer (loadSound/playSound/...) vía JNIEnv, y
 * esas llamadas caen en los stubs de este archivo (registrados en java.c).
 * La máquina de estados replica los códigos de GLMediaPlayer.java para que
 * el polling nativo (isSoundLoaded/getSoundStatus/...) se comporte igual:
 *   0 = vacío, 1 = descargado, 5 = cargado, 6 = sonando, 7 = detenido,
 *   8 = pausado.
 * Totales confirmados en el .so: nativeGetTotalSounds = 96,
 * nativeGetTotalMusics = 16 (ver out_ghidra.c).
 * Archivos: DATA_PATH hos/<id>.ogg y hos/m_<id>.ogg (Ogg Vorbis;
 * sonidos mono 22050 Hz, músicas stereo 44100 Hz). Se decodifican a PCM con
 * stb_vorbis (public domain, lib/stb_vorbis/) bajo demanda, igual que
 * MediaPlayer.create() en Java, y se mezclan a 44100 stereo en un hilo.
 */

#ifndef HOS_AUDIO_H
#define HOS_AUDIO_H

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_shutdown(void);

/* Drenaje de voces terminadas (== Java updateSound de onDrawFrame).
 * Llamado directo desde el loop, sin pasar por JNI. */
void audio_update_sound(void);

/* Sonidos (índices 0..95). */
jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args);
jint GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args);
jint GLMediaPlayer_getSoundStatus(jmethodID id, va_list args);
jint GLMediaPlayer_getSoundDuration(jmethodID id, va_list args);
void GLMediaPlayer_loadSound(jmethodID id, va_list args);
void GLMediaPlayer_playSound(jmethodID id, va_list args);
void GLMediaPlayer_pauseSound(jmethodID id, va_list args);
void GLMediaPlayer_resumeSound(jmethodID id, va_list args);
void GLMediaPlayer_stopSound(jmethodID id, va_list args);
void GLMediaPlayer_setSoundVolume(jmethodID id, va_list args);
void GLMediaPlayer_resetSound(jmethodID id, va_list args);
void GLMediaPlayer_unloadSound(jmethodID id, va_list args);
void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args);
void GLMediaPlayer_updateSound(jmethodID id, va_list args);

/* Músicas (índices 0..15). */
jint GLMediaPlayer_isMusicPlaying(jmethodID id, va_list args);
void GLMediaPlayer_loadMusic(jmethodID id, va_list args);
void GLMediaPlayer_playMusic(jmethodID id, va_list args);
void GLMediaPlayer_pauseMusic(jmethodID id, va_list args);
void GLMediaPlayer_resumeMusic(jmethodID id, va_list args);
void GLMediaPlayer_stopMusic(jmethodID id, va_list args);
void GLMediaPlayer_setMusicVolume(jmethodID id, va_list args);
void GLMediaPlayer_unloadMusic(jmethodID id, va_list args);
void GLMediaPlayer_stopAllMusic(jmethodID id, va_list args);

/* Película: no hay reproductor de video en Vita; se reporta como terminada
 * para que el motor la salte (ver IsVedioFinished). */
jint GLMediaPlayer_IsVedioFinished(jmethodID id, va_list args);
void GLMediaPlayer_ResumeMovie(jmethodID id, va_list args);
void GLMediaPlayer_loadMovie(jmethodID id, va_list args);
void GLMediaPlayer_loadMovieL(jmethodID id, va_list args);

#ifdef __cplusplus
}
#endif

#endif /* HOS_AUDIO_H */
