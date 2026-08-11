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
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);
extern void port_trace(const char *format, ...);

/* The Android build ships several DDS payloads with ATC FourCCs (ATC, ATCA
 * and ATCI), even though they keep a .tga filename inside 3d.pak.  VitaGL
 * cannot upload ATC directly.  Decode each 4x4 block to RGBA8 before handing
 * it to VitaGL so Purple can keep using its original compressed assets. */
#ifndef GL_ATC_RGB_AMD
#define GL_ATC_RGB_AMD 0x8C92
#endif
#ifndef GL_ATC_RGBA_EXPLICIT_ALPHA_AMD
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD 0x8C93
#endif
#ifndef GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD 0x87EE
#endif

static uint16_t atc_read_u16(const uint8_t *source) {
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t atc_read_u32(const uint8_t *source) {
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) |
           ((uint32_t)source[3] << 24);
}

static uint8_t atc_expand_5(unsigned value) {
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t atc_expand_6(unsigned value) {
    return (uint8_t)((value << 2) | (value >> 4));
}

static uint8_t atc_saturating_sub_quarter(uint8_t base, uint8_t other) {
    unsigned quarter = ((unsigned)other) >> 2;
    return base > quarter ? (uint8_t)(base - quarter) : 0;
}

static void atc_decode_color_block(const uint8_t *block,
                                   uint8_t colors[4][3],
                                   uint8_t indices[16]) {
    uint16_t packed0 = atc_read_u16(block);
    uint16_t packed1 = atc_read_u16(block + 2);
    GLboolean mode = (packed0 & 0x8000u) != 0;
    packed0 &= 0x7fffu;

    uint8_t color0[3] = {
        atc_expand_5((packed0 >> 10) & 31u),
        atc_expand_5((packed0 >> 5) & 31u),
        atc_expand_5(packed0 & 31u)
    };
    uint8_t color1[3] = {
        atc_expand_5((packed1 >> 11) & 31u),
        atc_expand_6((packed1 >> 5) & 63u),
        atc_expand_5(packed1 & 31u)
    };

    for (unsigned component = 0; component < 3; ++component) {
        if (mode) {
            colors[0][component] = 0;
            /* ATC differential mode palette order is black, the saturated
             * endpoint difference, endpoint 0 and endpoint 1.  Swapping
             * entries 1 and 3 produces blocky false-colour patches even
             * though the payload and dimensions are otherwise valid. */
            colors[1][component] = atc_saturating_sub_quarter(
                color0[component], color1[component]);
            colors[2][component] = color0[component];
            colors[3][component] = color1[component];
        } else {
            colors[0][component] = color0[component];
            /* ATC's regular-mode palette is endpoint 0, the 5:3 blend,
             * the 3:5 blend, then endpoint 1.  This is deliberately not the
             * DXT1 order.  Putting endpoint 1 in slot 1 makes adjacent 4x4
             * blocks select unrelated colours and visibly deforms faces,
             * hands and clothing. */
            colors[1][component] = (uint8_t)(
                (5u * color0[component] + 3u * color1[component]) >> 3);
            colors[2][component] = (uint8_t)(
                (3u * color0[component] + 5u * color1[component]) >> 3);
            colors[3][component] = color1[component];
        }
    }

    uint32_t packed_indices = atc_read_u32(block + 4);
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        indices[pixel] = (uint8_t)((packed_indices >> (pixel * 2)) & 3u);
    }
}

static void atc_decode_explicit_alpha(const uint8_t *block,
                                      uint8_t alpha[16]) {
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        uint8_t nibble = (uint8_t)((block[pixel >> 1] >>
            ((pixel & 1u) * 4u)) & 15u);
        alpha[pixel] = (uint8_t)(nibble * 17u);
    }
}

static void atc_decode_interpolated_alpha(const uint8_t *block,
                                          uint8_t alpha[16]) {
    uint8_t palette[8];
    palette[0] = block[0];
    palette[1] = block[1];
    if (palette[0] > palette[1]) {
        for (unsigned i = 1; i <= 6; ++i) {
            palette[i + 1] = (uint8_t)(
                ((7u - i) * palette[0] + i * palette[1] + 3u) / 7u);
        }
    } else {
        for (unsigned i = 1; i <= 4; ++i) {
            palette[i + 1] = (uint8_t)(
                ((5u - i) * palette[0] + i * palette[1] + 2u) / 5u);
        }
        palette[6] = 0;
        palette[7] = 255;
    }

    uint64_t packed_indices = 0;
    for (unsigned byte = 0; byte < 6; ++byte) {
        packed_indices |= ((uint64_t)block[2 + byte]) << (byte * 8);
    }
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        alpha[pixel] = palette[(packed_indices >> (pixel * 3)) & 7u];
    }
}

static GLboolean atc_decode_image(GLenum format, GLsizei width,
                                  GLsizei height, GLsizei image_size,
                                  const void *data, uint8_t *rgba) {
    if (!data || !rgba || width <= 0 || height <= 0) {
        return GL_FALSE;
    }

    unsigned block_bytes = format == GL_ATC_RGB_AMD ? 8u : 16u;
    unsigned blocks_x = ((unsigned)width + 3u) >> 2;
    unsigned blocks_y = ((unsigned)height + 3u) >> 2;
    size_t required = (size_t)blocks_x * blocks_y * block_bytes;
    if (image_size < 0 || (size_t)image_size < required) {
        port_trace("GL: ATC payload truncated format=0x%x size=%d required=%u",
                   format, image_size, (unsigned)required);
        return GL_FALSE;
    }

    const uint8_t *source = (const uint8_t *)data;
    for (unsigned block_y = 0; block_y < blocks_y; ++block_y) {
        for (unsigned block_x = 0; block_x < blocks_x; ++block_x) {
            const uint8_t *block = source +
                ((size_t)block_y * blocks_x + block_x) * block_bytes;
            const uint8_t *color_block = block;
            uint8_t alpha[16];
            if (format == GL_ATC_RGB_AMD) {
                memset(alpha, 255, sizeof(alpha));
            } else {
                color_block += 8;
                if (format == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD) {
                    atc_decode_explicit_alpha(block, alpha);
                } else {
                    atc_decode_interpolated_alpha(block, alpha);
                }
            }

            uint8_t colors[4][3];
            uint8_t indices[16];
            atc_decode_color_block(color_block, colors, indices);
            for (unsigned y = 0; y < 4; ++y) {
                unsigned output_y = block_y * 4u + y;
                if (output_y >= (unsigned)height) {
                    continue;
                }
                for (unsigned x = 0; x < 4; ++x) {
                    unsigned output_x = block_x * 4u + x;
                    if (output_x >= (unsigned)width) {
                        continue;
                    }
                    unsigned pixel = y * 4u + x;
                    uint8_t *output = rgba +
                        ((size_t)output_y * (unsigned)width + output_x) * 4u;
                    uint8_t color = indices[pixel];
                    output[0] = colors[color][0];
                    output[1] = colors[color][1];
                    output[2] = colors[color][2];
                    output[3] = alpha[pixel];
                }
            }
        }
    }
    return GL_TRUE;
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level,
                                     GLenum internal_format, GLsizei width,
                                     GLsizei height, GLint border,
                                     GLsizei image_size, const void *data) {
    if (internal_format != GL_ATC_RGB_AMD &&
        internal_format != GL_ATC_RGBA_EXPLICIT_ALPHA_AMD &&
        internal_format != GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD) {
        glCompressedTexImage2D(target, level, internal_format, width, height,
                               border, image_size, data);
        return;
    }

    if (width <= 0 || height <= 0) {
        port_trace("GL: invalid ATC dimensions level=%d %dx%d", level,
                   width, height);
        return;
    }

    size_t decoded_size = (size_t)width * (size_t)height * 4u;
    uint8_t *rgba = decoded_size ? (uint8_t *)malloc(decoded_size) : NULL;
    if (!rgba || !atc_decode_image(internal_format, width, height,
                                   image_size, data, rgba)) {
        port_trace("GL: ATC decode failed format=0x%x level=%d %dx%d bytes=%d",
                   internal_format, level, width, height, image_size);
        free(rgba);
        return;
    }

    static unsigned decode_count;
    unsigned current_decode = ++decode_count;
    if (level == 0 || current_decode <= 12)
        port_trace("GL: ATC decode #%u format=0x%x level=%d %dx%d bytes=%d -> RGBA8",
                   current_decode, internal_format, level, width, height,
                   image_size);
    glTexImage2D(target, level, GL_RGBA, width, height, border, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}

void glCompressedTexSubImage2D_soloader(GLenum target, GLint level,
                                        GLint xoffset, GLint yoffset,
                                        GLsizei width, GLsizei height,
                                        GLenum format, GLsizei image_size,
                                        const void *data) {
    static unsigned unsupported_traces;
    if (format != GL_ATC_RGB_AMD &&
        format != GL_ATC_RGBA_EXPLICIT_ALPHA_AMD &&
        format != GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD) {
        if (unsupported_traces++ < 8)
            port_trace("GL: compressed subimage unsupported format=0x%x "
                       "level=%d %dx%d", format, level, width, height);
        return;
    }

    if (width <= 0 || height <= 0)
        return;
    size_t decoded_size = (size_t)width * (size_t)height * 4u;
    uint8_t *rgba = (uint8_t *)malloc(decoded_size);
    if (!rgba || !atc_decode_image(format, width, height, image_size,
                                   data, rgba)) {
        port_trace("GL: ATC subimage decode failed format=0x%x level=%d "
                   "%dx%d bytes=%d", format, level, width, height,
                   image_size);
        free(rgba);
        return;
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}

void glBlendColor_soloader(GLfloat red, GLfloat green, GLfloat blue,
                           GLfloat alpha) {
    static unsigned trace_count;
    if (trace_count++ < 12)
        port_trace("GL: glBlendColor %.3f %.3f %.3f %.3f",
                   red, green, blue, alpha);
    /* VitaGL currently has no constant blend-color entry point.  Keep this
     * callback to expose actual game usage in the trace; glBlendFunc wrappers
     * below reveal whether a material also requests a constant factor. */
}

void glBlendFunc_soloader(GLenum source_factor, GLenum destination_factor) {
    static unsigned trace_count;
    if (trace_count++ < 32)
        port_trace("GL: glBlendFunc source=0x%x destination=0x%x",
                   source_factor, destination_factor);
    glBlendFunc(source_factor, destination_factor);
}

void glBlendFuncSeparate_soloader(GLenum source_rgb, GLenum destination_rgb,
                                  GLenum source_alpha,
                                  GLenum destination_alpha) {
    static unsigned trace_count;
    if (trace_count++ < 32)
        port_trace("GL: glBlendFuncSeparate rgb=0x%x,0x%x alpha=0x%x,0x%x",
                   source_rgb, destination_rgb, source_alpha,
                   destination_alpha);
    glBlendFuncSeparate(source_rgb, destination_rgb, source_alpha,
                        destination_alpha);
}

GLenum glCheckFramebufferStatus_soloader(GLenum target) {
    static unsigned trace_count;
    GLenum status = glCheckFramebufferStatus(target);
    if (status != GL_FRAMEBUFFER_COMPLETE || trace_count++ < 16)
        port_trace("GL: framebuffer target=0x%x status=0x%x", target, status);
    return status;
}

GLenum glGetError_soloader(void) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        port_trace("GL: glGetError -> 0x%x", error);
    return error;
}

static GLint mc2_upgrade_texture_filter(GLenum pname, GLint parameter) {
    if (pname == GL_TEXTURE_MAG_FILTER && parameter == GL_NEAREST)
        return GL_LINEAR;
    if (pname != GL_TEXTURE_MIN_FILTER)
        return parameter;
    switch (parameter) {
    case GL_NEAREST:
        return GL_LINEAR;
    case GL_NEAREST_MIPMAP_NEAREST:
    case GL_LINEAR_MIPMAP_NEAREST:
    case GL_NEAREST_MIPMAP_LINEAR:
        return GL_LINEAR_MIPMAP_LINEAR;
    default:
        return parameter;
    }
}

void glTexParameteri_soloader(GLenum target, GLenum pname, GLint parameter) {
    GLint upgraded = mc2_upgrade_texture_filter(pname, parameter);
    static unsigned trace_count;
    if ((pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) &&
        trace_count++ < 64)
        port_trace("GL: texture filter target=0x%x pname=0x%x requested=0x%x selected=0x%x",
                   target, pname, parameter, upgraded);
    glTexParameteri(target, pname, upgraded);
}

void glTexParameterf_soloader(GLenum target, GLenum pname, GLfloat parameter) {
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)
        glTexParameteri_soloader(target, pname, (GLint)parameter);
    else
        glTexParameterf(target, pname, parameter);
}

void glTexParameteriv_soloader(GLenum target, GLenum pname,
                               const GLint *parameters) {
    if (!parameters)
        return;
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)
        glTexParameteri_soloader(target, pname, parameters[0]);
    else
        glTexParameteriv(target, pname, parameters);
}

void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed parameter) {
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)
        glTexParameteri_soloader(target, pname, (GLint)parameter);
    else
        glTexParameterx(target, pname, parameter);
}

/* VitaGL's postponed GLSL path expects a vertex shader to already be attached
 * when glBindAttribLocation is called. GLES2 applications are allowed to bind
 * attribute locations at any time before linking, and Purple does so before
 * glAttachShader. Keep the requests here and replay them immediately before
 * glLinkProgram, after Purple has attached both shaders. */
#define MAX_DEFERRED_ATTRIB_BINDS 256
#define MAX_DEFERRED_ATTRIB_NAME 128
#define MAX_TRACKED_SHADERS 512
#define MAX_TRACKED_PROGRAMS 256

typedef struct deferred_attrib_bind {
    GLboolean used;
    GLuint program;
    GLuint index;
    GLchar name[MAX_DEFERRED_ATTRIB_NAME];
} deferred_attrib_bind;

static deferred_attrib_bind deferred_attrib_binds[MAX_DEFERRED_ATTRIB_BINDS];
static char *tracked_shader_sources[MAX_TRACKED_SHADERS + 1];
static GLenum tracked_shader_types[MAX_TRACKED_SHADERS + 1];
static GLuint tracked_shader_first_program[MAX_TRACKED_SHADERS + 1];
static GLuint tracked_vertex_shaders[MAX_TRACKED_PROGRAMS + 1];
static GLuint tracked_vertex_clones[MAX_TRACKED_PROGRAMS + 1];
static GLuint tracked_fragment_clones[MAX_TRACKED_PROGRAMS + 1];

static GLboolean is_identifier_character(char c) {
    return c == '_' || isalnum((unsigned char)c);
}

static GLboolean shader_source_has_identifier(const char *source,
                                               const char *name) {
    if (!source || !name || !name[0]) {
        return GL_FALSE;
    }

    const size_t name_length = strlen(name);
    const char *match = source;
    while ((match = strstr(match, name)) != NULL) {
        const char before = match == source ? '\0' : match[-1];
        const char after = match[name_length];
        if (!is_identifier_character(before) &&
            !is_identifier_character(after)) {
            return GL_TRUE;
        }
        match += name_length;
    }
    return GL_FALSE;
}

/* Purple emits a few tiny shaders through a generic material generator.  The
 * VitaGL GLSL translator used by the softfp package crashes while parsing the
 * local lowp texture-color temporary below.  Replacing it with the equivalent
 * single expression keeps the shader result unchanged and avoids that parser
 * path.  The caller reserves a small amount of headroom so compatibility
 * qualifiers can also be inserted safely. */
static GLboolean replace_source_fragment(char *source, size_t *length,
                                         size_t capacity,
                                         const char *needle,
                                         const char *replacement) {
    char *position = strstr(source, needle);
    if (!position) {
        return GL_FALSE;
    }

    const size_t needle_length = strlen(needle);
    const size_t replacement_length = strlen(replacement);
    const size_t new_length = *length - needle_length + replacement_length;
    if (new_length + 1 > capacity) {
        return GL_FALSE;
    }

    const size_t tail_length = strlen(position + needle_length) + 1;
    memmove(position + replacement_length, position + needle_length,
            tail_length);
    memcpy(position, replacement, replacement_length);
    *length = new_length;
    return GL_TRUE;
}

static void clear_tracked_shader(GLuint shader) {
    if (shader == 0 || shader > MAX_TRACKED_SHADERS) {
        return;
    }
    free(tracked_shader_sources[shader]);
    tracked_shader_sources[shader] = NULL;
    tracked_shader_types[shader] = 0;
    tracked_shader_first_program[shader] = 0;
}

static GLboolean track_shader_source_copy(GLuint shader, GLenum shader_type,
                                          const char *source,
                                          size_t length) {
    if (shader == 0 || shader > MAX_TRACKED_SHADERS || !source) {
        return GL_FALSE;
    }

    clear_tracked_shader(shader);
    tracked_shader_sources[shader] = malloc(length + 1);
    if (!tracked_shader_sources[shader]) {
        return GL_FALSE;
    }
    memcpy(tracked_shader_sources[shader], source, length);
    tracked_shader_sources[shader][length] = '\0';
    tracked_shader_types[shader] = shader_type;
    return GL_TRUE;
}

static void clear_deferred_attrib_binds(GLuint program) {
    for (unsigned i = 0; i < MAX_DEFERRED_ATTRIB_BINDS; ++i) {
        if (deferred_attrib_binds[i].used &&
            deferred_attrib_binds[i].program == program) {
            deferred_attrib_binds[i].used = GL_FALSE;
        }
    }
}

GLuint glCreateProgram_soloader(void) {
    GLuint program = glCreateProgram();
    if (program != 0) {
        clear_deferred_attrib_binds(program);
        if (program <= MAX_TRACKED_PROGRAMS) {
            tracked_vertex_shaders[program] = 0;
            tracked_vertex_clones[program] = 0;
            tracked_fragment_clones[program] = 0;
        }
    }
    port_trace("GL: glCreateProgram -> %u", program);
    return program;
}

void glDeleteProgram_soloader(GLuint program) {
    port_trace("GL: glDeleteProgram program=%u", program);
    if (program == 0) {
        return;
    }
    GLuint vertex_clone = 0;
    GLuint fragment_clone = 0;
    clear_deferred_attrib_binds(program);
    if (program <= MAX_TRACKED_PROGRAMS) {
        vertex_clone = tracked_vertex_clones[program];
        fragment_clone = tracked_fragment_clones[program];
        tracked_vertex_shaders[program] = 0;
        tracked_vertex_clones[program] = 0;
        tracked_fragment_clones[program] = 0;
    }
    glDeleteProgram(program);
    if (vertex_clone != 0) {
        glDeleteShader(vertex_clone);
        clear_tracked_shader(vertex_clone);
    }
    if (fragment_clone != 0 && fragment_clone != vertex_clone) {
        glDeleteShader(fragment_clone);
        clear_tracked_shader(fragment_clone);
    }
}

GLuint glCreateShader_soloader(GLenum shader_type) {
    GLuint shader = glCreateShader(shader_type);
    if (shader <= MAX_TRACKED_SHADERS) {
        clear_tracked_shader(shader);
        tracked_shader_types[shader] = shader_type;
    }
    port_trace("GL: glCreateShader type=0x%x -> %u", shader_type, shader);
    return shader;
}

void glDeleteShader_soloader(GLuint shader) {
    port_trace("GL: glDeleteShader shader=%u", shader);
    clear_tracked_shader(shader);
    if (shader != 0) {
        glDeleteShader(shader);
    }
}

void glAttachShader_soloader(GLuint program, GLuint shader) {
    port_trace("GL: glAttachShader program=%u shader=%u", program, shader);
    if (program == 0 || shader == 0) {
        port_trace("GL: glAttachShader ignored invalid zero handle");
        return;
    }
    const GLboolean tracked_source =
        program <= MAX_TRACKED_PROGRAMS &&
        shader <= MAX_TRACKED_SHADERS &&
        tracked_shader_sources[shader] &&
        (tracked_shader_types[shader] == GL_VERTEX_SHADER ||
         tracked_shader_types[shader] == GL_FRAGMENT_SHADER);
    const GLboolean reused_shader =
        tracked_source && tracked_shader_first_program[shader] != 0 &&
        tracked_shader_first_program[shader] != program;

    if (reused_shader) {
        const GLenum shader_type = tracked_shader_types[shader];
        const char *source = tracked_shader_sources[shader];
        const GLint source_length = (GLint)strlen(source);
        GLuint clone = glCreateShader(shader_type);
        if (clone != 0 && clone <= MAX_TRACKED_SHADERS &&
            track_shader_source_copy(clone, shader_type, source,
                                     (size_t)source_length)) {
            const GLchar *clone_source = tracked_shader_sources[clone];
            tracked_shader_first_program[clone] = program;
            glShaderSource(clone, 1, &clone_source, &source_length);
            glCompileShader(clone);
            glAttachShader(program, clone);

            GLuint *clone_slot;
            if (shader_type == GL_VERTEX_SHADER) {
                clone_slot = &tracked_vertex_clones[program];
                tracked_vertex_shaders[program] = clone;
            } else {
                clone_slot = &tracked_fragment_clones[program];
            }
            const GLuint old_clone = *clone_slot;
            *clone_slot = clone;
            if (old_clone != 0 && old_clone != clone) {
                glDeleteShader(old_clone);
                clear_tracked_shader(old_clone);
            }

            port_trace("GL: cloned shader program=%u original=%u clone=%u type=0x%x",
                       program, shader, clone, shader_type);
            port_trace("GL: glAttachShader completed program=%u shader=%u clone=%u",
                       program, shader, clone);
            return;
        }
        if (clone != 0) {
            glDeleteShader(clone);
            clear_tracked_shader(clone);
        }
        port_trace("GL: shader clone failed; attaching original=%u", shader);
    }
    if (tracked_source && tracked_shader_first_program[shader] == 0) {
        tracked_shader_first_program[shader] = program;
        port_trace("GL: first shader attachment program=%u shader=%u",
                   program, shader);
    }
    if (program <= MAX_TRACKED_PROGRAMS &&
        shader <= MAX_TRACKED_SHADERS &&
        tracked_shader_types[shader] == GL_VERTEX_SHADER) {
        tracked_vertex_shaders[program] = shader;
    }
    glAttachShader(program, shader);
    port_trace("GL: glAttachShader completed program=%u shader=%u",
               program, shader);
}

void glBindAttribLocation_soloader(GLuint program, GLuint index,
                                   const GLchar *name) {
    port_trace("GL: glBindAttribLocation deferred program=%u index=%u name=%s",
               program, index, name ? name : "(null)");
    if (program == 0 || !name) {
        port_trace("GL: glBindAttribLocation ignored invalid arguments");
        return;
    }

    for (unsigned i = 0; i < MAX_DEFERRED_ATTRIB_BINDS; ++i) {
        if (!deferred_attrib_binds[i].used) {
            deferred_attrib_binds[i].used = GL_TRUE;
            deferred_attrib_binds[i].program = program;
            deferred_attrib_binds[i].index = index;
            strncpy(deferred_attrib_binds[i].name, name,
                    MAX_DEFERRED_ATTRIB_NAME - 1);
            deferred_attrib_binds[i].name[MAX_DEFERRED_ATTRIB_NAME - 1] = '\0';
            return;
        }
    }

    port_trace("GL: deferred attribute table full; binding was not queued");
}

void glLinkProgram_soloader(GLuint program) {
    unsigned replayed = 0;
    unsigned skipped = 0;
    const char *vertex_source = NULL;
    port_trace("GL: glLinkProgram program=%u enter", program);
    if (program == 0) {
        port_trace("GL: glLinkProgram ignored invalid zero handle");
        return;
    }

    if (program <= MAX_TRACKED_PROGRAMS) {
        const GLuint vertex_shader = tracked_vertex_shaders[program];
        if (vertex_shader != 0 && vertex_shader <= MAX_TRACKED_SHADERS) {
            vertex_source = tracked_shader_sources[vertex_shader];
        }
    }

    for (unsigned i = 0; i < MAX_DEFERRED_ATTRIB_BINDS; ++i) {
        deferred_attrib_bind *bind = &deferred_attrib_binds[i];
        if (!bind->used || bind->program != program) {
            continue;
        }

        if (vertex_source &&
            !shader_source_has_identifier(vertex_source, bind->name)) {
            port_trace("GL: skip inactive attrib program=%u index=%u name=%s",
                       program, bind->index, bind->name);
            bind->used = GL_FALSE;
            ++skipped;
            continue;
        }

        port_trace("GL: replay attrib program=%u index=%u name=%s",
                   program, bind->index, bind->name);
        glBindAttribLocation(program, bind->index, bind->name);
        bind->used = GL_FALSE;
        ++replayed;
    }

    port_trace("GL: glLinkProgram program=%u replayed=%u skipped=%u",
               program, replayed, skipped);
    glLinkProgram(program);
    port_trace("GL: glLinkProgram program=%u native link returned", program);
}

void glGetProgramiv_soloader(GLuint program, GLenum pname, GLint *params) {
    port_trace("GL: glGetProgramiv program=%u pname=0x%x", program, pname);
    if (program == 0 || !params) {
        if (params) {
            *params = GL_FALSE;
        }
        port_trace("GL: glGetProgramiv ignored invalid arguments");
        return;
    }
    glGetProgramiv(program, pname, params);
    port_trace("GL: glGetProgramiv program=%u pname=0x%x -> %d",
               program, pname, *params);
}

void gl_preload() {
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
    /* Purple compiles its own splash shaders immediately after EGL setup.
     * Let VitaGL's asynchronous splash/initialization worker finish first;
     * ShaccCg is not safe when two startup paths enter it concurrently. */
    vglInitExtended(0, 960, 544, 6 * 1024 * 1024,
                    SCE_GXM_MULTISAMPLE_NONE);
    port_trace("GL: VitaGL init returned; waiting for startup worker");
    sceKernelDelayThread(4000 * 1000);
    port_trace("GL: startup serialization delay completed");
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

/* Purple queries the GL identity immediately after eglMakeCurrent.  The v6
 * core stopped in that transition before the engine could draw its splash.
 * Return immutable GLES2 capability strings without entering VitaGL's
 * information path during its own splashscreen/GC startup. */
const GLubyte *glGetString_soloader(GLenum name) {
    const char *value;
    switch (name) {
        case GL_VENDOR:
            value = "VitaGL";
            break;
        case GL_RENDERER:
            value = "PowerVR SGX 543MP4+ (VitaGL)";
            break;
        case GL_VERSION:
            value = "OpenGL ES 2.0 VitaGL";
            break;
        case GL_SHADING_LANGUAGE_VERSION:
            value = "OpenGL ES GLSL ES 1.00";
            break;
        case GL_EXTENSIONS:
            value =
                "GL_AMD_compressed_ATC_texture "
                "GL_OES_compressed_ETC1_RGB8_texture "
                "GL_IMG_texture_compression_pvrtc "
                "GL_OES_depth24 "
                "GL_OES_depth_texture "
                "GL_OES_element_index_uint "
                "GL_OES_framebuffer_object "
                "GL_OES_mapbuffer "
                "GL_OES_packed_depth_stencil "
                "GL_OES_rgb8_rgba8 "
                "GL_OES_standard_derivatives "
                "GL_EXT_blend_minmax "
                "GL_EXT_blend_subtract "
                "GL_EXT_discard_framebuffer "
                "GL_EXT_texture_filter_anisotropic";
            break;
        default:
            value = "";
            break;
    }
    port_trace("GL: glGetString name=0x%x -> %.48s", name, value);
    return (const GLubyte *)value;
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
    static unsigned source_call;
    unsigned call = ++source_call;
    port_trace("GL: glShaderSource #%u shader=%u count=%d lengths=%p",
               call, shader, count, _length);
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

    /* Keep room for the compatibility rewrites inserted below. */
    const size_t source_capacity = total_length + 256 + 1;
    char * str = malloc(source_capacity);
    if (!str) {
        l_error("<%p> Could not allocate %u bytes for shader source",
                __builtin_return_address(0), (unsigned)source_capacity);
        skip_next_compile = GL_TRUE;
        return;
    }
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

    static const char texture_color_temporary[] =
        "lowp vec4 color=texture2D(tex0,vUv);color*=vCol;"
        "gl_FragColor=color;";
    static const char texture_color_direct[] =
        "gl_FragColor=texture2D(tex0,vUv)*vCol;";
    if (replace_source_fragment(str, &total_length, source_capacity,
                                texture_color_temporary,
                                texture_color_direct)) {
        port_trace("GL: shader #%u rewrote texture-color temporary", call);
    }

    /* Purple's generated vertex-color shader leaves aColor at the default
     * mediump precision and sends it directly to a lowp varying.  Keep the
     * input and output precision matched so the softfp VitaGL/ShaRK path does
     * not produce an invalid GXM program for this valid GLES conversion. */
    static const char vertex_color_default[] =
        "attribute vec4 aColor;varying lowp vec4 vCol;";
    static const char vertex_color_lowp[] =
        "attribute lowp vec4 aColor;varying lowp vec4 vCol;";
    if (replace_source_fragment(str, &total_length, source_capacity,
                                vertex_color_default,
                                vertex_color_lowp)) {
        port_trace("GL: shader #%u normalized aColor to lowp", call);
    }

    /* One Purple material combines the model-view-projection transform and
     * vertex color in a very small vertex shader.  The softfp VitaGL GLSL
     * translator deterministically walks past its parser state on that exact
     * statement sequence.  Program 3 uses the same operations followed by a
     * scale and offset and translates correctly, so express identity scale
     * and offset explicitly here.  This preserves every output value while
     * keeping the translator on its known-good path. */
    static const char mvp_vertex_color_compact[] =
        "gl_Position=gModelViewProjMatrix*aPosition;vCol=aColor;";
    static const char mvp_vertex_color_safe[] =
        "gl_Position=gModelViewProjMatrix*aPosition;"
        "gl_Position*=vec4(1,1,1,1);"
        "gl_Position+=vec4(0,0,0,0);"
        "vCol=aColor;";
    if (replace_source_fragment(str, &total_length, source_capacity,
                                mvp_vertex_color_compact,
                                mvp_vertex_color_safe)) {
        port_trace("GL: shader #%u normalized MVP vertex-color path", call);
    }

    port_trace("GL: glShaderSource #%u bytes=%u prefix=%.80s", call,
               (unsigned)total_length, str);
    if (call <= 12 && total_length < 700) {
        port_trace("GL: glShaderSource #%u full=<<<%s>>>", call, str);
    }

    if (shader != 0 && shader <= MAX_TRACKED_SHADERS) {
        track_shader_source_copy(shader, tracked_shader_types[shader], str,
                                 total_length);
    }

    load_shader(shader, str, total_length);

    port_trace("GL: glShaderSource #%u completed", call);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
    static unsigned compile_call;
    unsigned call = ++compile_call;
    port_trace("GL: glCompileShader #%u shader=%u enter", call, shader);
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
        port_trace("GL: glCompileShader #%u native compile returned", call);
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
    port_trace("GL: glCompileShader #%u leave", call);
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
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
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
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
