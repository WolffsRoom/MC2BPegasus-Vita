#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/init.h"
#include "utils/logger.h"

#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"
#include "reimpl/audio.h"
#include "reimpl/intro_video.h"

extern void port_trace(const char *format, ...);
extern void port_trace_flush(void);

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

typedef void (*mc2_void_fn)(JNIEnv *, jobject);
typedef void (*mc2_info_fn)(JNIEnv *, jobject, jstring, jstring, jstring,
                            jstring);
typedef void (*mc2_renderer_init_fn)(JNIEnv *, jobject, jint, jint, jint,
                                     jstring);
typedef void (*mc2_resize_fn)(JNIEnv *, jobject, jint, jint);
typedef void (*mc2_key_fn)(JNIEnv *, jobject, jint);
typedef int (*mc2_app_event_fn)(void *, const void *);
typedef void (*mc2_media_init_fn)(JNIEnv *, jobject, jint);
typedef struct Mc2TouchPoint {
    float x;
    float y;
} Mc2TouchPoint;
typedef void (*mc2_touch_point_fn)(void *, const Mc2TouchPoint *, long);

typedef struct Mc2TouchEvent {
    int event_type;
    int pointer_id;
    int x;
    int y;
    int reserved;
    int touch_type;
} Mc2TouchEvent;
typedef char Mc2TouchEvent_size_must_be_24[
    sizeof(Mc2TouchEvent) == 24 ? 1 : -1];

static int dummy_game_object;
static jobject game_object = (jobject)&dummy_game_object;
static mc2_void_fn game_native_init;
static mc2_void_fn renderer_native_render;
static mc2_void_fn renderer_native_done;
static mc2_void_fn surface_native_pause;
static mc2_void_fn surface_native_resume;
static mc2_key_fn game_native_key_down;
static mc2_key_fn game_native_key_up;
static mc2_app_event_fn application_on_event;
static mc2_touch_point_fn touchscreen_touch_began;
static mc2_touch_point_fn touchscreen_touch_moved;
static mc2_touch_point_fn touchscreen_touch_ended;
static volatile int *engine_touch_enabled;
static volatile int *engine_slide_up;
static volatile int *engine_initialized;
static volatile int *engine_resume_timer;
static volatile int *engine_texture_count;
static void **engine_device;
static void **engine_application;
static int touch_render_pending;

static uintptr_t required_symbol(const char *name) {
    uintptr_t address = so_symbol(&so_mod, name);
    if (!address)
        fatal_error("Required libsandstorm2.so symbol is missing: %s", name);
    port_trace("main: resolved %s -> %p", name, (void *)address);
    return address;
}

static void write_raw_boot_marker(void) {
    static const char marker[] =
        "Modern Combat 2 Vita diagnostic v24 reached main()\n";
    SceUID fd = sceIoOpen("ux0:data/mc2_boot.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, marker, sizeof(marker) - 1);
        sceIoClose(fd);
    }
}

static void trace_data_preflight(void) {
    static const char * const required_paths[] = {
        DATA_PATH "libsandstorm2.so",
        DATA_PATH "GloftBPHP/data/3d.header",
        DATA_PATH "GloftBPHP/data/3d.pak",
        DATA_PATH "GloftBPHP/data/2d.header",
        DATA_PATH "GloftBPHP/data/intro/logo-vita.mp4",
    };

    for (unsigned index = 0;
         index < sizeof(required_paths) / sizeof(required_paths[0]); ++index) {
        SceIoStat stat = {0};
        int result = sceIoGetstat(required_paths[index], &stat);
        port_trace("preflight: %s result=0x%x size=%lld",
                   required_paths[index], result,
                   result >= 0 ? (long long)stat.st_size : -1LL);
    }
}

static void resolve_game_entrypoints(void) {
    game_native_init = (mc2_void_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeInit");
    renderer_native_render = (mc2_void_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeRender");
    renderer_native_done = (mc2_void_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeDone");
    surface_native_pause = (mc2_void_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameGLSurfaceView_nativePause");
    surface_native_resume = (mc2_void_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameGLSurfaceView_nativeResume");
    game_native_key_down = (mc2_key_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeOnKeyDown");
    game_native_key_up = (mc2_key_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeOnKeyUp");
    application_on_event = (mc2_app_event_fn)required_symbol(
        "_ZN11Application7onEventERKN6glitch6SEventE");
    touchscreen_touch_began = (mc2_touch_point_fn)required_symbol(
        "_ZN15TouchScreenBase10touchBeganERKN6glitch4core8vector2dIfEEl");
    touchscreen_touch_moved = (mc2_touch_point_fn)required_symbol(
        "_ZN15TouchScreenBase10touchMovedERKN6glitch4core8vector2dIfEEl");
    touchscreen_touch_ended = (mc2_touch_point_fn)required_symbol(
        "_ZN15TouchScreenBase10touchEndedERKN6glitch4core8vector2dIfEEl");
    engine_touch_enabled = (volatile int *)so_symbol(&so_mod, "isEnableTouch");
    engine_slide_up = (volatile int *)so_symbol(&so_mod, "m_isSlideUp");
    engine_initialized = (volatile int *)so_symbol(&so_mod, "initialized");
    engine_resume_timer = (volatile int *)so_symbol(&so_mod, "m_timerForResume");
    engine_texture_count = (volatile int *)so_symbol(&so_mod, "countTexture");
    engine_device = (void **)so_symbol(&so_mod, "device");
    engine_application = (void **)so_symbol(&so_mod, "app");
    port_trace("main: touch engine Application::onEvent=%p enable=%p slide=%p",
               (void *)application_on_event, (void *)engine_touch_enabled,
               (void *)engine_slide_up);
}

static void trace_engine_input_state(const char *where) {
    void *application = engine_application ? *engine_application : NULL;
    void *touchscreen = application ?
        *(void **)((char *)application + 0x50) : NULL;
    port_trace("INPUTSTATE: %s initialized=%d resumeTimer=%d textures=%d "
               "device=%p app=%p touchscreen=%p enabled=%d slide=%d",
               where,
               engine_initialized ? *engine_initialized : -1,
               engine_resume_timer ? *engine_resume_timer : -1,
               engine_texture_count ? *engine_texture_count : -1,
               engine_device ? *engine_device : NULL,
               application, touchscreen,
               engine_touch_enabled ? *engine_touch_enabled : -1,
               engine_slide_up ? *engine_slide_up : -1);
}

static void force_physical_input_mode(void) {
    if (engine_touch_enabled)
        *engine_touch_enabled = 1;
    if (engine_slide_up && !touch_render_pending)
        *engine_slide_up = 0;
}

static void begin_touch_render_mode(void) {
    /* Game.onTouchEvent passes its slide field (2) to nativeOnTouch before
     * posting the event.  The engine consumes the queued SEvent later in the
     * following nativeRender, so keep this mode alive for that whole call. */
    touch_render_pending = 1;
    if (engine_touch_enabled)
        *engine_touch_enabled = 1;
    if (engine_slide_up)
        *engine_slide_up = 2;
}

static int prepare_input_mode_for_render(void) {
    static unsigned trace_count;
    if (!touch_render_pending)
        return 0;
    if (engine_touch_enabled)
        *engine_touch_enabled = 1;
    if (engine_slide_up)
        *engine_slide_up = 2;
    if (trace_count++ < 16)
        port_trace("INPUTMODE: touch render begin enabled=%d slide=%d",
                   engine_touch_enabled ? *engine_touch_enabled : -1,
                   engine_slide_up ? *engine_slide_up : -1);
    return 1;
}

static void finish_input_mode_after_render(int rendered_touch) {
    static unsigned trace_count;
    if (!rendered_touch)
        return;
    /* Physical Xperia Play key dispatch only works with the slide closed.
     * Restore it after the exact render that consumed the touch queue. */
    if (engine_slide_up)
        *engine_slide_up = 0;
    touch_render_pending = 0;
    if (trace_count++ < 16)
        port_trace("INPUTMODE: touch render end enabled=%d slide=%d",
                   engine_touch_enabled ? *engine_touch_enabled : -1,
                   engine_slide_up ? *engine_slide_up : -1);
}

static void bootstrap_game(void) {
    mc2_info_fn native_get_info = (mc2_info_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeGetInfo");
    mc2_renderer_init_fn renderer_init =
        (mc2_renderer_init_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeInit");
    mc2_resize_fn renderer_resize = (mc2_resize_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeResize");
    mc2_media_init_fn media_init = (mc2_media_init_fn)required_symbol(
        "Java_com_gameloft_android_GAND_GloftBPHP_ML_GLMediaPlayer_nativeInit");

    jstring country = jni->NewStringUTF(&jni, "US");
    jstring build = jni->NewStringUTF(&jni, "GL_00");
    jstring device = jni->NewStringUTF(&jni, "sony ericsson_R800i");
    jstring android_release = jni->NewStringUTF(&jni, "2.3.4");
    jstring version = jni->NewStringUTF(&jni, "1.0.0");

    port_trace("main: calling Game.nativeGetInfo");
    native_get_info(&jni, game_object, country, build, device,
                    android_release);
    port_trace("main: Game.nativeGetInfo returned");

    /* The supplied data set targets the Xperia Play R800 profile.  The Java
     * original returns manufacturer profile 4 for Sony devices, followed by
     * the surface width, height and package version. */
    port_trace("main: calling GameRenderer.nativeInit profile=%d 960x544",
               MC2_TEXTURE_PROFILE);
    renderer_init(&jni, game_object, MC2_TEXTURE_PROFILE, 960, 544, version);
    port_trace("main: GameRenderer.nativeInit returned");

    port_trace("main: calling GLMediaPlayer.nativeInit mode=0");
    media_init(&jni, game_object, 0);
    port_trace("main: GLMediaPlayer.nativeInit returned");

    port_trace("main: calling Game.nativeInit");
    game_native_init(&jni, game_object);
    port_trace("main: Game.nativeInit returned");

    renderer_resize(&jni, game_object, 960, 544);
    port_trace("main: GameRenderer.nativeResize returned");
    surface_native_resume(&jni, game_object);
    port_trace("main: GameGLSurfaceView.nativeResume returned");
    force_physical_input_mode();
    port_trace("main: touchscreen forced enabled=%d slide=%d",
               engine_touch_enabled ? *engine_touch_enabled : -1,
               engine_slide_up ? *engine_slide_up : -1);
    trace_engine_input_state("bootstrap");
}

int main(void) {
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    write_raw_boot_marker();
    sceIoMkdir(DATA_PATH "files", 0777);
    sceIoRemove(DATA_PATH "port.log");
    port_trace("Modern Combat 2 Vita port trace v24 "
               "(fixed AVPlayer handle lifetime, H264 intro and audio guard)");
    port_trace("main: process entered, DATA_PATH=%s", DATA_PATH);
    trace_data_preflight();

    soloader_init_all();
    port_trace("main: loader initialization completed");
    port_trace_flush();
    resolve_game_entrypoints();

    port_trace("main: initializing VitaGL 960x544");
    gl_init();
    port_trace("main: VitaGL initialized");
    port_trace_flush();
    mc2_play_intro_video();
    bootstrap_game();
    port_trace_flush();

    const uint64_t target_frame_us = 16667;
    const unsigned stats_interval = 600;
    port_trace("main: entering 60 FPS renderer loop target_us=%llu",
               (unsigned long long)target_frame_us);
    unsigned frame = 0;
    unsigned missed_frames = 0;
    uint64_t stats_started = sceKernelGetProcessTimeWide();
    uint64_t render_total = 0;
    uint64_t render_max = 0;
    while (1) {
        uint64_t start = sceKernelGetProcessTimeWide();
        controls_poll();
        int rendered_touch = prepare_input_mode_for_render();
        renderer_native_render(&jni, game_object);
        finish_input_mode_after_render(rendered_touch);
        gl_swap();

        uint64_t render_elapsed = sceKernelGetProcessTimeWide() - start;
        render_total += render_elapsed;
        if (render_elapsed > render_max)
            render_max = render_elapsed;
        if (render_elapsed > target_frame_us)
            ++missed_frames;

        if (++frame <= 8)
            port_trace("main: rendered frame=%u", frame);

        if (render_elapsed < target_frame_us)
            sceKernelDelayThread((unsigned)(target_frame_us - render_elapsed));

        if (frame % stats_interval == 0) {
            uint64_t now = sceKernelGetProcessTimeWide();
            uint64_t window = now - stats_started;
            unsigned fps_x100 = window ? (unsigned)(
                ((uint64_t)stats_interval * 100000000u) / window) : 0;
            port_trace("PERF: fps=%u.%02u render_avg_us=%llu "
                       "render_max_us=%llu missed=%u/%u",
                       fps_x100 / 100, fps_x100 % 100,
                       (unsigned long long)(render_total / stats_interval),
                       (unsigned long long)render_max, missed_frames,
                       stats_interval);
            mc2_audio_log_stats();
            port_trace_flush();
            stats_started = now;
            render_total = 0;
            render_max = 0;
            missed_frames = 0;
        } else if (frame % 120 == 0) {
            port_trace_flush();
        }
    }

    surface_native_pause(&jni, game_object);
    renderer_native_done(&jni, game_object);
    sceKernelExitProcess(0);
    return 0;
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    if (!game_native_key_down || !game_native_key_up)
        return;
    /* A touchscreen contact must not leave the Xperia Play slide state set;
     * Application::OnKeyDown ignores the physical route in that mode. */
    force_physical_input_mode();
    if (action == CONTROLS_ACTION_DOWN) {
        port_trace("INPUT: key down code=%d", keycode);
        game_native_key_down(&jni, game_object, keycode);
    } else if (action == CONTROLS_ACTION_UP) {
        port_trace("INPUT: key up code=%d", keycode);
        game_native_key_up(&jni, game_object, keycode);
    }
}

static int dispatch_application_touch(int32_t id, jint x, jint y,
                                      ControlsAction action) {
    if (!application_on_event || !engine_application || !*engine_application)
        return 0;

    /* Exact 24-byte glitch::SEvent touch layout written by appOnTouch:
     * event type 1, pointer id, integer coordinates and touch 0/6/3 for
     * began/moved/ended.  Calling Application::onEvent bypasses only the
     * disconnected IDevice user-receiver hop. */
    Mc2TouchEvent event = {
        .event_type = 1,
        .pointer_id = id,
        .x = x,
        .y = y,
        .reserved = 0,
        .touch_type = action == CONTROLS_ACTION_DOWN ? 0 :
                      action == CONTROLS_ACTION_UP ? 3 : 6,
    };
    return application_on_event(*engine_application, &event);
}

static int dispatch_camera_touch(int32_t id, float x, float y,
                                 ControlsAction action) {
    /* The renderer may clear isEnableTouch after each frame.  Reassert it for
     * every analog sample, but keep slide=0 so this path never selects the
     * virtual-control HUD.  force_physical_input_mode preserves slide=2 if a
     * real front-panel contact is pending in the same frame. */
    force_physical_input_mode();
    void *application = engine_application ? *engine_application : NULL;
    void *touchscreen = application ?
        *(void **)((char *)application + 0x50) : NULL;
    if (!touchscreen)
        return 0;

    Mc2TouchPoint point = { x, y };
    if (action == CONTROLS_ACTION_DOWN)
        touchscreen_touch_began(touchscreen, &point, id);
    else if (action == CONTROLS_ACTION_UP)
        touchscreen_touch_ended(touchscreen, &point, id);
    else
        touchscreen_touch_moved(touchscreen, &point, id);
    return 1;
}

void controls_handler_touch(int32_t id, float x, float y,
                            ControlsAction action) {
    if (!application_on_event)
        return;
    static unsigned move_traces;
    static int state_traced;
    if (!state_traced) {
        trace_engine_input_state("first-touch-before");
        state_traced = 1;
    }
    if (action != CONTROLS_ACTION_MOVE || move_traces++ < 12)
        port_trace("INPUT: application touch type=%d x=%d y=%d id=%d",
                   action == CONTROLS_ACTION_DOWN ? 0 :
                   action == CONTROLS_ACTION_UP ? 3 : 6,
                   (jint)x, (jint)y, id);
    begin_touch_render_mode();
    int handled = dispatch_application_touch(id, (jint)x, (jint)y, action);
    if (action != CONTROLS_ACTION_MOVE)
        port_trace("INPUT: Application::onEvent handled=%d", handled);
    if (action == CONTROLS_ACTION_DOWN) {
        trace_engine_input_state("touch-down-after");
    }
}

static int analog_x_key;
static int analog_y_key;

static void update_analog_key(int *current, int next) {
    if (*current == next)
        return;
    if (*current)
        game_native_key_up(&jni, game_object, *current);
    *current = next;
    if (*current)
        game_native_key_down(&jni, game_object, *current);
}

void controls_handler_analog(ControlsStickId which, float x, float y,
                             ControlsAction action) {
    if (!game_native_key_down || !game_native_key_up)
        return;

    if (which == CONTROLS_STICK_LEFT) {
        int horizontal = x < -0.35f ? AKEYCODE_DPAD_LEFT :
                         x > 0.35f ? AKEYCODE_DPAD_RIGHT : 0;
        int vertical = y < -0.35f ? AKEYCODE_DPAD_UP :
                       y > 0.35f ? AKEYCODE_DPAD_DOWN : 0;
        update_analog_key(&analog_x_key, horizontal);
        update_analog_key(&analog_y_key, vertical);
        return;
    }
    if (!touchscreen_touch_began || !touchscreen_touch_moved ||
        !touchscreen_touch_ended || !engine_application ||
        !*engine_application)
        return;

    static int right_touch_active;
    static float right_touch_x = 720.0f;
    static float right_touch_y = 272.0f;
    static unsigned right_touch_traces;
    /* poll_stick reports MOVE while the stick is idle.  The old IDevice path
     * silently filtered it; the direct Application path must never receive a
     * MOVE/UP for pointer 1 before its DOWN, or TouchScreenBase dereferences a
     * missing touch point. */
    if (action == CONTROLS_ACTION_MOVE && !right_touch_active)
        return;
    if (action == CONTROLS_ACTION_UP && !right_touch_active)
        return;
    if (action == CONTROLS_ACTION_DOWN) {
        right_touch_active = 1;
        right_touch_x = 720.0f;
        right_touch_y = 272.0f;
        force_physical_input_mode();
        dispatch_camera_touch(1, 720.0f, 272.0f, CONTROLS_ACTION_DOWN);
        if (right_touch_traces++ < 24)
            port_trace("INPUT: right analog DOWN center=720,272 stick=%.3f,%.3f",
                       x, y);
        return;
    }

    if (action == CONTROLS_ACTION_UP) {
        dispatch_camera_touch(1, right_touch_x, right_touch_y,
                              CONTROLS_ACTION_UP);
        force_physical_input_mode();
        if (right_touch_traces++ < 24)
            port_trace("INPUT: right analog UP x=%d y=%d",
                       (jint)right_touch_x, (jint)right_touch_y);
        right_touch_active = 0;
        right_touch_x = 720.0f;
        right_touch_y = 272.0f;
        return;
    }

    /* A camera drag is relative.  The old implementation mapped stick
     * deflection to one fixed absolute coordinate, so every subsequent MOVE
     * had zero displacement.  Accumulate a small swipe every rendered frame
     * instead.  Restart the synthetic contact before it reaches the edge so a
     * held stick can keep rotating indefinitely. */
    /* Input is polled at 60 Hz, so halve the per-frame displacement to
     * preserve the camera speed that was tuned at 30 Hz. */
    const float delta_x = x * 7.0f;
    const float delta_y = y * 6.0f;
    float next_x = right_touch_x + delta_x;
    float next_y = right_touch_y + delta_y;
    if (next_x < 500.0f || next_x > 940.0f ||
        next_y < 24.0f || next_y > 520.0f) {
        dispatch_camera_touch(1, right_touch_x, right_touch_y,
                              CONTROLS_ACTION_UP);
        right_touch_x = 720.0f;
        right_touch_y = 272.0f;
        dispatch_camera_touch(1, 720.0f, 272.0f, CONTROLS_ACTION_DOWN);
        next_x = right_touch_x + delta_x;
        next_y = right_touch_y + delta_y;
        if (right_touch_traces++ < 24)
            port_trace("INPUT: right analog recentered stick=%.3f,%.3f", x, y);
    }
    right_touch_x = next_x;
    right_touch_y = next_y;
    dispatch_camera_touch(1, right_touch_x, right_touch_y,
                          CONTROLS_ACTION_MOVE);
    if (right_touch_traces++ < 24)
        port_trace("INPUT: right analog MOVE x=%d y=%d stick=%.3f,%.3f",
                   (jint)right_touch_x, (jint)right_touch_y, x, y);
}
