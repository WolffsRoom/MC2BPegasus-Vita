#include "reimpl/intro_video.h"

#include "utils/glutil.h"

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>

#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void port_trace(const char *format, ...);
extern void port_trace_flush(void);

enum {
    INTRO_STALL_TIMEOUT_US = 8 * 1000 * 1000,
    INTRO_TOTAL_TIMEOUT_US = 95 * 1000 * 1000,
    INTRO_FRAME_ALLOCS = 8,
    INTRO_PROBE_BYTES = 4 * 1024 * 1024,
    INTRO_PROBE_CHUNK = 64 * 1024,
};

typedef struct {
    void *pointer;
    uint32_t size;
} IntroFrameAllocation;

static IntroFrameAllocation frame_allocations[INTRO_FRAME_ALLOCS];
static pthread_mutex_t frame_allocations_mutex = PTHREAD_MUTEX_INITIALIZER;

/* On retail Vita, sceAvPlayerInit returns an internal user-memory address in
 * the 0x81xxxxxx..0x9xxxxxxx range as its 32-bit handle.  SceAvPlayerHandle
 * is typedef'd as int, so the usual `handle < 0` error test misclassifies a
 * perfectly valid pointer such as 0x82e0b000. */
static int intro_is_valid_player_handle(SceAvPlayerHandle handle) {
    uint32_t value = (uint32_t)handle;
    return value >= 0x81000000u && value < 0xa0000000u && !(value & 3u);
}

static int intro_buffer_has_tag(const uint8_t *buffer, uint32_t size,
                                const char tag[4]) {
    if (size < 4)
        return 0;
    for (uint32_t i = 0; i <= size - 4; ++i) {
        if (!memcmp(buffer + i, tag, 4))
            return 1;
    }
    return 0;
}

/* SceAvPlayer's controller thread aborts the whole process when this game's
 * original MPEG-4 Visual (mp4v) stream is submitted.  A normal return-code
 * check or watchdog cannot recover that asynchronous kernel-player crash.
 * Only accept the fast-start H.264/AAC file made by prepare-video.ps1. */
static int intro_is_safe_avc_aac_mp4(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;

    uint8_t *buffer = malloc(INTRO_PROBE_CHUNK + 3);
    if (!buffer) {
        sceIoClose(fd);
        return 0;
    }

    uint32_t carry = 0;
    uint32_t inspected = 0;
    int has_ftyp = 0;
    int has_avc1 = 0;
    int has_mp4a = 0;
    int has_mp4v = 0;
    while (inspected < INTRO_PROBE_BYTES) {
        uint32_t wanted = INTRO_PROBE_CHUNK;
        if (wanted > INTRO_PROBE_BYTES - inspected)
            wanted = INTRO_PROBE_BYTES - inspected;
        int read = sceIoRead(fd, buffer + carry, wanted);
        if (read <= 0)
            break;
        uint32_t available = carry + (uint32_t)read;
        has_ftyp |= intro_buffer_has_tag(buffer, available, "ftyp");
        has_avc1 |= intro_buffer_has_tag(buffer, available, "avc1");
        has_mp4a |= intro_buffer_has_tag(buffer, available, "mp4a");
        has_mp4v |= intro_buffer_has_tag(buffer, available, "mp4v");
        inspected += (uint32_t)read;
        carry = available < 3 ? available : 3;
        if (carry)
            memmove(buffer, buffer + available - carry, carry);
    }

    free(buffer);
    sceIoClose(fd);
    port_trace("INTRO: codec probe bytes=%u ftyp=%d avc1=%d mp4a=%d mp4v=%d",
               inspected, has_ftyp, has_avc1, has_mp4a, has_mp4v);
    port_trace_flush();
    return has_ftyp && has_avc1 && has_mp4a && !has_mp4v;
}

static void *intro_allocate(void *unused, uint32_t alignment, uint32_t size) {
    (void)unused;
    if (alignment < 16)
        alignment = 16;
    return memalign(alignment, size);
}

static void intro_deallocate(void *unused, void *pointer) {
    (void)unused;
    free(pointer);
}

static void *intro_allocate_frame(void *unused, uint32_t alignment,
                                  uint32_t size) {
    void *pointer = intro_allocate(unused, alignment, size);
    if (!pointer)
        return NULL;
    pthread_mutex_lock(&frame_allocations_mutex);
    for (unsigned i = 0; i < INTRO_FRAME_ALLOCS; ++i) {
        if (!frame_allocations[i].pointer) {
            frame_allocations[i].pointer = pointer;
            frame_allocations[i].size = size;
            break;
        }
    }
    pthread_mutex_unlock(&frame_allocations_mutex);
    port_trace("INTRO: frame allocation pointer=%p size=%u alignment=%u",
               pointer, size, alignment);
    return pointer;
}

static void intro_deallocate_frame(void *unused, void *pointer) {
    pthread_mutex_lock(&frame_allocations_mutex);
    for (unsigned i = 0; i < INTRO_FRAME_ALLOCS; ++i) {
        if (frame_allocations[i].pointer == pointer) {
            frame_allocations[i] = (IntroFrameAllocation){0};
            break;
        }
    }
    pthread_mutex_unlock(&frame_allocations_mutex);
    intro_deallocate(unused, pointer);
}

static uint32_t intro_frame_allocation_size(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uint32_t size = 0;
    pthread_mutex_lock(&frame_allocations_mutex);
    for (unsigned i = 0; i < INTRO_FRAME_ALLOCS; ++i) {
        uintptr_t start = (uintptr_t)frame_allocations[i].pointer;
        uintptr_t end = start + frame_allocations[i].size;
        if (start && address >= start && address < end) {
            size = frame_allocations[i].size - (uint32_t)(address - start);
            break;
        }
    }
    pthread_mutex_unlock(&frame_allocations_mutex);
    return size;
}

static int clamp_byte(int value) {
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return value;
}

/* SceAvPlayer exposes decoded video as NV12 (YUV420, two planes).  The frame
 * allocation size lets us recover the decoder pitch, including the padding
 * used for widths such as this movie's 854 pixels. */
static void nv12_to_rgba(const uint8_t *source, uint32_t allocation_size,
                         uint32_t width, uint32_t height, uint8_t *rgba) {
    uint32_t aligned_height = (height + 15u) & ~15u;
    uint32_t pitch = width;
    if (allocation_size) {
        uint64_t candidate = ((uint64_t)allocation_size * 2u) /
                             ((uint64_t)aligned_height * 3u);
        if (candidate >= width && candidate <= width + 256u)
            pitch = (uint32_t)candidate;
    }
    if (pitch == width)
        pitch = (width + 15u) & ~15u;

    const uint8_t *luma = source;
    const uint8_t *chroma = source + (size_t)pitch * aligned_height;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *y_row = luma + (size_t)y * pitch;
        const uint8_t *uv_row = chroma + (size_t)(y / 2) * pitch;
        uint8_t *destination = rgba + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            int yy = (int)y_row[x] - 16;
            int u = (int)uv_row[x & ~1u] - 128;
            int v = (int)uv_row[(x & ~1u) + 1] - 128;
            if (yy < 0)
                yy = 0;
            int c = 298 * yy;
            destination[0] = (uint8_t)clamp_byte((c + 409 * v + 128) >> 8);
            destination[1] = (uint8_t)clamp_byte(
                (c - 100 * u - 208 * v + 128) >> 8);
            destination[2] = (uint8_t)clamp_byte((c + 516 * u + 128) >> 8);
            destination[3] = 255;
            destination += 4;
        }
    }
}

static void draw_intro_frame(GLuint texture, uint32_t width,
                             uint32_t height, const uint8_t *rgba,
                             int first_frame) {
    const GLfloat vertices[] = {
        0.0f, 2.0f, 0.0f,
        960.0f, 2.0f, 0.0f,
        0.0f, 542.0f, 0.0f,
        960.0f, 542.0f, 0.0f,
    };
    const GLfloat coordinates[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (first_frame) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width,
                     (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width,
                        (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, 960.0f, 544.0f, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor4ub(255, 255, 255, 255);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    gl_swap();
}

static int intro_skip_requested(uint32_t *previous_buttons,
                                int *touch_was_down) {
    SceCtrlData pad = {0};
    int samples = sceCtrlPeekBufferPositive(0, &pad, 1);
    if (samples > 0) {
        const uint32_t skip_buttons = SCE_CTRL_CROSS | SCE_CTRL_CIRCLE |
                                      SCE_CTRL_START;
        uint32_t pressed = pad.buttons & ~*previous_buttons;
        *previous_buttons = pad.buttons;
        if (pressed & skip_buttons)
            return 1;
    }

    SceTouchData touch = {0};
    samples = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (samples > 0) {
        int down = touch.reportNum > 0;
        int pressed = down && !*touch_was_down;
        *touch_was_down = down;
        if (pressed)
            return 1;
    }
    return 0;
}

void mc2_play_intro_video(void) {
    const char *path = DATA_PATH "GloftBPHP/data/intro/logo-vita.mp4";
    SceIoStat stat = {0};
    if (sceIoGetstat(path, &stat) < 0 || stat.st_size <= 0) {
        port_trace("INTRO: safe H264 movie missing; original mp4v is never "
                   "opened; continuing without video: %s", path);
        port_trace_flush();
        return;
    }
    if (!intro_is_safe_avc_aac_mp4(path)) {
        port_trace("INTRO: rejected file without safe avc1/mp4a signature; "
                   "continuing without video: %s", path);
        port_trace_flush();
        return;
    }

    port_trace("INTRO: loading AVPlayer module for validated H264/AAC source");
    port_trace_flush();
    int module_result = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (module_result < 0) {
        port_trace("INTRO: AVPlayer module load failed=0x%x; skipped",
                   module_result);
        port_trace_flush();
        return;
    }

    pthread_mutex_lock(&frame_allocations_mutex);
    memset(frame_allocations, 0, sizeof(frame_allocations));
    pthread_mutex_unlock(&frame_allocations_mutex);
    SceAvPlayerInitData init = {0};
    init.memoryReplacement.allocate = intro_allocate;
    init.memoryReplacement.deallocate = intro_deallocate;
    init.memoryReplacement.allocateTexture = intro_allocate_frame;
    init.memoryReplacement.deallocateTexture = intro_deallocate_frame;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 3;
    init.autoStart = SCE_TRUE;
    init.defaultLanguage = "eng";

    port_trace("INTRO: calling sceAvPlayerInit");
    port_trace_flush();
    SceAvPlayerHandle player = sceAvPlayerInit(&init);
    port_trace("INTRO: sceAvPlayerInit returned raw handle=0x%08x",
               (uint32_t)player);
    port_trace_flush();
    if (!intro_is_valid_player_handle(player)) {
        port_trace("INTRO: init returned non-handle value=0x%08x; skipped "
                   "without unloading AVPlayer", (uint32_t)player);
        port_trace_flush();
        /* Init may have created a controller thread before reporting failure.
         * Keep the module mapped until process exit; unloading it here made
         * that thread execute unmapped code and caused Prefetch Abort. */
        return;
    }

    port_trace("INTRO: calling sceAvPlayerAddSource path=%s size=%lld",
               path, (long long)stat.st_size);
    port_trace_flush();
    int source_result = sceAvPlayerAddSource(player, path);
    port_trace("INTRO: add source result=0x%x path=%s size=%lld",
               source_result, path, (long long)stat.st_size);
    port_trace_flush();
    if (source_result < 0) {
        int close_result = sceAvPlayerClose(player);
        port_trace("INTRO: source rejected; close result=0x%x; module kept",
                   close_result);
        port_trace_flush();
        return;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    uint8_t *rgba = NULL;
    uint32_t rgba_width = 0;
    uint32_t rgba_height = 0;
    int first_frame = 1;
    int audio_port = -1;
    unsigned decoded_frames = 0;
    unsigned audio_frames = 0;
    int ever_active = 0;
    uint32_t previous_buttons = 0;
    int touch_was_down = 0;
    uint64_t started = sceKernelGetProcessTimeWide();
    uint64_t last_progress = started;
    uint64_t last_player_time = 0;

    port_trace("INTRO: playback started; Cross, Circle, Start or touch skips");
    port_trace_flush();
    for (;;) {
        uint64_t now = sceKernelGetProcessTimeWide();
        int active = sceAvPlayerIsActive(player);
        if (active)
            ever_active = 1;
        if (intro_skip_requested(&previous_buttons, &touch_was_down)) {
            port_trace("INTRO: skipped by user at %llu ms",
                       (unsigned long long)sceAvPlayerCurrentTime(player));
            break;
        }
        if (now - started > INTRO_TOTAL_TIMEOUT_US) {
            port_trace("INTRO: total watchdog expired; continuing game");
            break;
        }
        if (now - last_progress > INTRO_STALL_TIMEOUT_US) {
            port_trace("INTRO: stalled for 8 seconds; continuing game");
            break;
        }
        if (!active && (ever_active || now - started > 3 * 1000 * 1000))
            break;

        SceAvPlayerFrameInfo video = {0};
        if (sceAvPlayerGetVideoData(player, &video)) {
            uint32_t width = video.details.video.width;
            uint32_t height = video.details.video.height;
            if (video.pData && width && height && width <= 1920 &&
                height <= 1088) {
                if (width != rgba_width || height != rgba_height) {
                    free(rgba);
                    rgba = malloc((size_t)width * height * 4);
                    rgba_width = rgba ? width : 0;
                    rgba_height = rgba ? height : 0;
                    first_frame = 1;
                    port_trace("INTRO: video stream %ux%u frame_buffer=%u",
                               width, height,
                               intro_frame_allocation_size(video.pData));
                }
                if (rgba) {
                    nv12_to_rgba(video.pData,
                                 intro_frame_allocation_size(video.pData),
                                 width, height, rgba);
                    draw_intro_frame(texture, width, height, rgba,
                                     first_frame);
                    first_frame = 0;
                    ++decoded_frames;
                    last_progress = now;
                }
            }
        }

        SceAvPlayerFrameInfo audio = {0};
        if (sceAvPlayerGetAudioData(player, &audio) && audio.pData) {
            uint32_t channels = audio.details.audio.channelCount;
            uint32_t rate = audio.details.audio.sampleRate;
            uint32_t size = audio.details.audio.size;
            uint32_t samples_per_channel = channels ?
                size / (channels * sizeof(int16_t)) : 0;
            if (audio_port == -1) {
                if (channels == 2 && rate == 48000 &&
                    samples_per_channel >= 64 &&
                    samples_per_channel <= 65472 &&
                    !(samples_per_channel & 63)) {
                    int opened_port = sceAudioOutOpenPort(
                        SCE_AUDIO_OUT_PORT_TYPE_MAIN, samples_per_channel,
                        rate, SCE_AUDIO_OUT_MODE_STEREO);
                    port_trace("INTRO: audio stream channels=%u rate=%u "
                               "samples=%u port=%d", channels, rate,
                               samples_per_channel, opened_port);
                    audio_port = opened_port >= 0 ? opened_port : -2;
                } else {
                    port_trace("INTRO: unsupported audio channels=%u rate=%u "
                               "samples=%u; video continues muted", channels,
                               rate, samples_per_channel);
                    audio_port = -2;
                }
            }
            if (audio_port >= 0) {
                int output_result = sceAudioOutOutput(audio_port, audio.pData);
                if (output_result < 0) {
                    port_trace("INTRO: audio output failed=0x%x; muted",
                               output_result);
                    sceAudioOutReleasePort(audio_port);
                    audio_port = -2;
                } else {
                    ++audio_frames;
                    last_progress = now;
                }
            }
        }

        uint64_t player_time = sceAvPlayerCurrentTime(player);
        if (player_time != last_player_time) {
            last_player_time = player_time;
            last_progress = now;
        }
        sceKernelDelayThread(1000);
    }

    if (audio_port >= 0)
        sceAudioOutReleasePort(audio_port);
    int stop_result = sceAvPlayerStop(player);
    int close_result = sceAvPlayerClose(player);
    if (texture)
        glDeleteTextures(1, &texture);
    free(rgba);
    port_trace("INTRO: finished video_frames=%u audio_frames=%u time=%llu ms "
               "stop=0x%x close=0x%x; AVPlayer module kept until process exit",
               decoded_frames, audio_frames,
               (unsigned long long)last_player_time, stop_result, close_result);
    port_trace_flush();
}
