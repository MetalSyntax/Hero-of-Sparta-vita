/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb/stb_image.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> gl_preload() glutil.c:%d", __LINE__); }
#endif
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> gl_init() glutil.c:%d", __LINE__); }
#endif
    // Igual que los hermanos GLES1 que sí dibujan (Asphalt-5, GT Racing): sin MSAA, 12 MB.
    vglInitExtended(0, 960, 544, 12 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    /* La lógica del motor avanza un paso por frame (Main::Render), así que hay
     * que fijar el ritmo: 2 vblanks = 30 FPS estables (3 = 20, más cerca de los
     * 45 ms/frame del GameRenderer de Android). */
    extern uint32_t vsync_interval;
    vsync_interval = 2;
}

/* El motor HD está hecho para tablets Honeycomb: GLRenderer::Init/Zoom/
 * SetOrthoProjection hardcodean glViewport(0,0,1280,752) y glOrthof(0,1280,752,0).
 * Todo rect que pide está en ese espacio; se reescala a la pantalla real. */
static void rescale_rect(GLint *x, GLint *y, GLsizei *w, GLsizei *h) {
    *x = *x * 960 / ENGINE_W;
    *y = *y * 544 / ENGINE_H;
    *w = *w * 960 / ENGINE_W;
    *h = *h * 544 / ENGINE_H;
}

/* Diagnóstico bug #2 (pantalla negra con el motor vivo): contadores por
 * intervalo que gl_heartbeat() vuelca al log. Dicen si el motor dibuja algo. */
static unsigned s_draws, s_clears, s_texups, s_frames, s_viewports, s_draws_total;

void glViewport_soloader(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (s_viewports++ < 8)
        l_info("[gl] glViewport(%d,%d,%d,%d)", x, y, w, h);
    rescale_rect(&x, &y, &w, &h);
    glViewport(x, y, w, h);
}

void glScissor_soloader(GLint x, GLint y, GLsizei w, GLsizei h) {
    rescale_rect(&x, &y, &w, &h);
    glScissor(x, y, w, h);
}

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    s_draws++;
    s_draws_total++;
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    s_draws++;
    s_draws_total++;
    glDrawElements(mode, count, type, idx);
}

void glClear_soloader(GLbitfield mask) {
    s_clears++;
    glClear(mask);
}

/* Texture::Load decodifica PVRTC por software (PVRTCDecompress → RGBA) y sube
 * el resultado con glTexImage2D: minutos de CPU en la carga. El hook solo anota
 * el bloque comprimido; glTexImage2D_soloader lo reconoce por el buffer de salida
 * y lo sube tal cual a la GPU (vitaGL soporta PVRTC nativo). */
static const void *s_pvr_src;
static void *s_pvr_out;
static int s_pvr_2bpp, s_pvr_w, s_pvr_h;
static unsigned s_pvr_native;

void PVRTCDecompress_hook(const void *src, int do2bit, int w, int h, unsigned char *out) {
    s_pvr_src = src;
    s_pvr_out = out;
    s_pvr_2bpp = do2bit;
    s_pvr_w = w;
    s_pvr_h = h;
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                           GLint border, GLenum fmt, GLenum type, const void *data) {
    if (data && data == s_pvr_out && w == s_pvr_w && h == s_pvr_h) {
        GLsizei size = s_pvr_2bpp ? (w < 16 ? 16 : w) * (h < 8 ? 8 : h) / 4
                                  : (w < 8 ? 8 : w) * (h < 8 ? 8 : h) / 2;
        GLenum cfmt = s_pvr_2bpp ? GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG
                                 : GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG;
        s_pvr_out = NULL;
        glGetError(); /* descarta el error viejo de glDisable(GL_GENERATE_MIPMAP) */
        glCompressedTexImage2D(target, level, cfmt, w, h, 0, size, s_pvr_src);
        GLenum err = glGetError();
        if (s_pvr_native++ < 8 || err)
            l_info("[gl] PVRTC nativo %dx%d %dbpp -> 0x%x", w, h, s_pvr_2bpp ? 2 : 4, err);
        s_texups++;
        return;
    }
    if (s_texups++ < 16)
        l_info("[gl] glTexImage2D(lvl=%d ifmt=0x%x %dx%d fmt=0x%x type=0x%x data=%p)",
               level, ifmt, w, h, fmt, type, data);
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
}

/* Pantalla de carga: el motor no dibuja nada propio (0 draws) hasta el menú, así
 * que se muestra el logo de Gameloft del APK (res/drawable/gi_gameloft_logo.png)
 * centrado a tamaño nativo sobre negro.
 * Se dibuja encima del clear del motor hasta su primer draw real, guardando y
 * restaurando todo estado GL que toca para no ensuciar el del motor. */
void gl_draw_splash(void) {
    static int state; /* 0 = sin cargar, 1 = listo, 2 = terminado */
    static GLuint tex;
    static const GLfloat tc[] = { 0, 1, 1, 1, 0, 0, 1, 0 };
    static GLfloat vtx[8];
    static const GLenum caps[] = { GL_TEXTURE_2D, GL_BLEND, GL_DEPTH_TEST, GL_ALPHA_TEST,
                                   GL_CULL_FACE, GL_LIGHTING, GL_FOG, GL_SCISSOR_TEST };
    static const GLenum arrays[] = { GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY,
                                     GL_COLOR_ARRAY, GL_NORMAL_ARRAY };
    GLboolean cap_on[8], arr_on[4];
    GLint prev_tex, vp[4];

    if (state == 2)
        return;
    if (s_draws_total) {
        if (tex)
            glDeleteTextures(1, &tex);
        state = 2;
        return;
    }
    if (state == 0) {
        int w, h, c;
        unsigned char *px = stbi_load(DATA_PATH "res/drawable/gi_gameloft_logo.png", &w, &h, &c, 4);
        state = 1;
        if (!px) {
            l_warn("[gl] sin " DATA_PATH "res/drawable/gi_gameloft_logo.png: carga en negro");
            state = 2;
            return;
        }
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindTexture(GL_TEXTURE_2D, prev_tex);
        stbi_image_free(px);
        /* Centrado (origen abajo-izquierda): x0,y0, x1,y0, x0,y1, x1,y1. */
        vtx[0] = vtx[4] = (960 - w) / 2.0f;
        vtx[2] = vtx[6] = vtx[0] + w;
        vtx[1] = vtx[3] = (544 - h) / 2.0f;
        vtx[5] = vtx[7] = vtx[1] + h;
    }

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_VIEWPORT, vp);
    for (int i = 0; i < 8; i++)
        cap_on[i] = glIsEnabled(caps[i]);
    for (int i = 0; i < 4; i++)
        arr_on[i] = glIsEnabled(arrays[i]);

    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, 960, 0, 544, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    for (int i = 0; i < 8; i++)
        glDisable(caps[i]);
    glEnable(GL_TEXTURE_2D);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexPointer(2, GL_FLOAT, 0, vtx);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindTexture(GL_TEXTURE_2D, prev_tex);
    for (int i = 0; i < 8; i++)
        (cap_on[i] ? glEnable : glDisable)(caps[i]);
    for (int i = 0; i < 4; i++)
        (arr_on[i] ? glEnableClientState : glDisableClientState)(arrays[i]);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void gl_heartbeat(void) {
    static unsigned last_ms;
    unsigned now = sceKernelGetProcessTimeLow() / 1000;
    s_frames++;
    if (now - last_ms < 3000)
        return;
    l_info("[gl] heartbeat: %u frames, %u draws, %u clears, %u texups total, glGetError=0x%x",
           s_frames, s_draws, s_clears, s_texups, glGetError());
    s_frames = s_draws = s_clears = 0;
    last_ms = now;
}

// vitaGL no trae glLightx; solo params escalares (spot/atenuación) en 16.16.
void glLightx_soloader(GLenum light, GLenum pname, GLfixed param) {
    GLfloat f = param / 65536.0f;
    glLightfv(light, pname, &f);
}

void gl_swap() {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> gl_swap() glutil.c:%d", __LINE__); }
#endif
    vglSwapBuffers(GL_FALSE);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> glCompileShader_soloader() glutil.c:%d", __LINE__); }
#endif
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> load_shader() glutil.c:%d", __LINE__); }
#endif
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> load_shader() glutil.c:%d", __LINE__); }
#endif
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> load_shader() glutil.c:%d", __LINE__); }
#endif
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
#ifdef PORT_TRACE
    { /* PORT_TRACE_AUTO */ l_debug("[TRACE] >> load_shader() glutil.c:%d", __LINE__); }
#endif
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
