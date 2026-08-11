/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/egl.h"

#include "utils/glutil.h"
#include "utils/logger.h"

#include <string.h>
#include <stdlib.h>

extern void port_trace(const char *format, ...);

static EGLBoolean egl_runtime_initialized = EGL_FALSE;
static EGLDisplay egl_current_display;
static EGLSurface egl_current_draw;
static EGLSurface egl_current_read;
static EGLContext egl_current_context;

EGLBoolean bbr_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    l_debug("eglInitialize(0x%x)", (int)dpy);

    port_trace("EGL: eglInitialize display=%p initialized=%u", dpy,
               egl_runtime_initialized);
    if (!egl_runtime_initialized) {
        gl_init();
        egl_runtime_initialized = EGL_TRUE;
        port_trace("EGL: VitaGL initialized");
    }

    if (major) *major = 2;
    if (minor) *minor = 2;

    return EGL_TRUE;
}

EGLBoolean bbr_eglQueryContext(EGLDisplay dpy, EGLContext ctx,
                               EGLint attribute, EGLint *value) {
    port_trace("EGL: eglQueryContext display=%p ctx=%p attr=0x%x", dpy,
               ctx, attribute);
    EGLBoolean ret = EGL_TRUE;
    switch (attribute) {
        case EGL_CONFIG_ID:
            *value = 0;
            break;
        case EGL_CONTEXT_CLIENT_TYPE:
            *value = EGL_OPENGL_ES_API;
            break;
        case EGL_CONTEXT_CLIENT_VERSION:
            *value = 2;
            break;
        case EGL_RENDER_BUFFER:
            *value = EGL_BACK_BUFFER;
            break;
        default:
            l_error("eglQueryContext / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            ret = EGL_FALSE;
            break;
    }

    port_trace("EGL: eglQueryContext attr=0x%x result=%u value=%d",
               attribute, ret, value ? *value : -1);
    return ret;
}


EGLBoolean bbr_eglQuerySurface(EGLDisplay dpy, EGLSurface eglSurface,
                               EGLint attribute, EGLint *value) {
    EGLBoolean ret = EGL_TRUE;
    switch (attribute) {
        case EGL_CONFIG_ID:
            *value = 0;
            break;
        case EGL_WIDTH:
            *value = 960;
            break;
        case EGL_HEIGHT:
            *value = 544;
            break;
        case EGL_TEXTURE_FORMAT:
            *value = EGL_TEXTURE_RGBA;
            break;
        case EGL_TEXTURE_TARGET:
            *value = EGL_TEXTURE_2D;
            break;
        case EGL_SWAP_BEHAVIOR:
            *value = EGL_BUFFER_PRESERVED;
            break;
        case EGL_LARGEST_PBUFFER:
        case EGL_MIPMAP_TEXTURE:
            *value = EGL_FALSE;
            break;
        case EGL_MIPMAP_LEVEL:
            *value = 0;
            break;
        case EGL_MULTISAMPLE_RESOLVE:
            // ignored when creating the surface, return default
            *value = EGL_MULTISAMPLE_RESOLVE_DEFAULT;
            break;
        case EGL_HORIZONTAL_RESOLUTION:
        case EGL_VERTICAL_RESOLUTION:
            *value = 220 * EGL_DISPLAY_SCALING; // VITA DPI is 220
            break;
        case EGL_PIXEL_ASPECT_RATIO:
            // Please don't ask why * EGL_DISPLAY_SCALING, the document says it
            *value = 960 / 544 * EGL_DISPLAY_SCALING;
            break;
        case EGL_RENDER_BUFFER:
            *value = EGL_BACK_BUFFER;
            break;
        case EGL_VG_COLORSPACE:
            // ignored when creating the surface, return default
            *value = EGL_VG_COLORSPACE_sRGB;
            break;
        case EGL_VG_ALPHA_FORMAT:
            // ignored when creating the surface, return default
            *value = EGL_VG_ALPHA_FORMAT_NONPRE;
            break;
        case EGL_TIMESTAMPS_ANDROID:
            *value = EGL_FALSE;
            break;
        default:
            l_error("eglQuerySurface / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            ret = EGL_FALSE;
            break;
    }

    return ret;
}


EGLBoolean bbr_eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                                  EGLint attribute, EGLint *value) {
    switch (attribute) {
        case EGL_ALPHA_SIZE: {
            *value = 8;
            break;
        }
        case EGL_ALPHA_MASK_SIZE: {
            *value = 8;
            break;
        }
        case EGL_BIND_TO_TEXTURE_RGB: {
            *value = EGL_TRUE;
            break;
        }
        case EGL_BIND_TO_TEXTURE_RGBA: {
            *value = EGL_TRUE;
            break;
        }
        case EGL_BLUE_SIZE: {
            *value = 8;
            break;
        }
        case EGL_BUFFER_SIZE: {
            *value = 32;
            break;
        }
        case EGL_COLOR_BUFFER_TYPE: {
            *value = EGL_RGB_BUFFER;
            break;
        }
        case EGL_CONFIG_CAVEAT: {
            *value = EGL_NONE;
            break;
        }
        case EGL_CONFIG_ID: {
            *value = 0;
            break;
        }
        case EGL_CONFORMANT: {
            *value = EGL_OPENGL_ES2_BIT;
            break;
        }
        case EGL_DEPTH_SIZE: {
            *value = 24;
            break;
        }
        case EGL_GREEN_SIZE: {
            *value = 8;
            break;
        }
        case EGL_LEVEL: {
            *value = 0;
            break;
        }
        case EGL_LUMINANCE_SIZE: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_WIDTH: {
            *value = 960;
            break;
        }
        case EGL_MAX_PBUFFER_HEIGHT: {
            *value = 544;
            break;
        }
        case EGL_MAX_PBUFFER_PIXELS: {
            *value = 960 * 544;
            break;
        }
        case EGL_MAX_SWAP_INTERVAL: {
            *value = 1;
            break;
        }
        case EGL_MIN_SWAP_INTERVAL: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_RENDERABLE: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_VISUAL_ID: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_VISUAL_TYPE: {
            *value = 0;
            break;
        }
        case EGL_RED_SIZE: {
            *value = 8;
            break;
        }
        case EGL_RENDERABLE_TYPE: {
            *value = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_BIT;
            break;
        }
        case EGL_SAMPLE_BUFFERS: {
            *value = 0;
            break;
        }
        case EGL_SAMPLES: {
            *value = 0;
            break;
        }
        case EGL_STENCIL_SIZE: {
            *value = 8;
            break;
        }
        case EGL_SURFACE_TYPE: {
            *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
            break;
        }
        case EGL_TRANSPARENT_TYPE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_RED_VALUE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_GREEN_VALUE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_BLUE_VALUE: {
            *value = 0;
            break;
        }
        default:
            l_error("eglGetConfigAttrib / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean bbr_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size,
                               EGLint *num_config) {
    if (!num_config) {
        return EGL_BAD_PARAMETER;
    }

    if (!configs || config_size <= 0) {
        *num_config = 1;
        return EGL_TRUE;
    }

    *configs = strdup("conf");
    *num_config = 1;

    return EGL_TRUE;
}

EGLContext bbr_eglCreateContext(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context,
                                const EGLint *attrib_list) {
    // VitaGL owns the real graphics context; the Android side only needs a
    // stable opaque EGL handle whose identity survives eglMakeCurrent.
    EGLContext context = strdup("ctx");
    port_trace("EGL: eglCreateContext display=%p share=%p -> %p", dpy,
               share_context, context);
    return context;
}

EGLSurface bbr_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                      void *win,
                                      const EGLint *attrib_list) {
    EGLSurface surface = strdup("surface");
    port_trace("EGL: eglCreateWindowSurface display=%p window=%p -> %p",
               dpy, win, surface);
    return surface;
}

EGLSurface bbr_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list) {
    (void)dpy; (void)config; (void)attrib_list;
    EGLSurface surface = strdup("pbuffer");
    port_trace("EGL: eglCreatePbufferSurface display=%p -> %p", dpy,
               surface);
    return surface;
}

EGLBoolean bbr_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                              EGLSurface read, EGLContext ctx) {
    egl_current_display = dpy;
    egl_current_draw = draw;
    egl_current_read = read;
    egl_current_context = ctx;
    port_trace("EGL: eglMakeCurrent display=%p draw=%p read=%p ctx=%p",
               dpy, draw, read, ctx);
    return EGL_TRUE;
}

EGLBoolean bbr_eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    port_trace("EGL: eglDestroyContext display=%p ctx=%p", dpy, ctx);
    if (egl_current_context == ctx)
        egl_current_context = NULL;
    if (ctx) free(ctx);
    return EGL_TRUE;
}

EGLBoolean bbr_eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    port_trace("EGL: eglDestroySurface display=%p surface=%p", dpy,
               surface);
    if (egl_current_draw == surface) egl_current_draw = NULL;
    if (egl_current_read == surface) egl_current_read = NULL;
    if (surface) free(surface);
    return EGL_TRUE;
}

EGLBoolean bbr_eglTerminate(EGLDisplay dpy) {
    port_trace("EGL: eglTerminate display=%p", dpy);
    if (egl_current_display == dpy) {
        egl_current_display = NULL;
        egl_current_draw = NULL;
        egl_current_read = NULL;
        egl_current_context = NULL;
    }
    return EGL_TRUE;
}

EGLContext bbr_eglGetCurrentContext(void) {
    return egl_current_context;
}

char const *bbr_eglQueryString(EGLDisplay display, EGLint name) {
    switch (name) {
    case EGL_CLIENT_APIS:
        return "OpenGL OpenGL_ES";
    case EGL_VENDOR:
        return "Rinnegatamante";
    case EGL_VERSION:
        return "2.2 VitaGL";
    case EGL_EXTENSIONS:
        return "EGL_KHR_image "
               "EGL_KHR_image_base "
               "EGL_KHR_image_pixmap "
               "EGL_KHR_gl_texture_2D_image "
               "EGL_KHR_gl_texture_cubemap_image "
               "EGL_KHR_gl_renderbuffer_image "
               "EGL_KHR_fence_sync "
               "EGL_NV_system_time "
               "EGL_ANDROID_image_native_buffer ";
    default:
        return NULL;
    }
}

EGLBoolean bbr_eglGetConfigs(EGLDisplay display, EGLConfig *configs,
                             EGLint config_size, EGLint *num_config) {
    if (!num_config) {
        l_error("eglGetConfigs / EGL_BAD_PARAMETER");
        return EGL_FALSE;
    }

    if (configs && config_size > 0) {
        *configs = strdup("conf");
    }

    *num_config = 1;

    return EGL_TRUE;
}
