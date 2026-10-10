/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

/* Resolución para la que está hecho el motor (ver glViewport_soloader). */
#define ENGINE_W 1280
#define ENGINE_H 752

void glViewport_soloader(GLint x, GLint y, GLsizei w, GLsizei h);

void glScissor_soloader(GLint x, GLint y, GLsizei w, GLsizei h);

void glLightx_soloader(GLenum light, GLenum pname, GLfixed param);

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *idx);

void glClear_soloader(GLbitfield mask);

void glTexImage2D_soloader(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                           GLint border, GLenum fmt, GLenum type, const void *data);

void gl_heartbeat(void);

void gl_draw_splash(void);

void PVRTCDecompress_hook(const void *src, int do2bit, int w, int h, unsigned char *out);

void glCompileShader_soloader(GLuint shader);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
