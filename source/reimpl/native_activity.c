#include "reimpl/native_activity.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

extern void port_trace(const char *format, ...);

enum {
    AINPUT_EVENT_TYPE_KEY = 1,
    AINPUT_EVENT_TYPE_MOTION = 2,
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001002,
    AINPUT_SOURCE_GAMEPAD = 0x00000401,
    AINPUT_SOURCE_JOYSTICK = 0x01000010,
    AKEY_EVENT_ACTION_DOWN = 0,
    AKEY_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_DOWN = 0,
    AMOTION_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_MOVE = 2,
    AMOTION_EVENT_AXIS_X = 0,
    AMOTION_EVENT_AXIS_Y = 1,
    AMOTION_EVENT_AXIS_Z = 11,
    AMOTION_EVENT_AXIS_RZ = 14,
    APP_CMD_INIT_WINDOW = 1,
    APP_CMD_GAINED_FOCUS = 6,
    APP_CMD_START = 10,
    APP_CMD_RESUME = 11,
    ALOOPER_POLL_TIMEOUT = -3,
};

struct ALooper { int unused; };
struct AInputQueue { int unused; };
struct ANativeWindow { int32_t width, height, format; };
struct AConfiguration { char language[2], country[2]; };
struct ASensorManager { int unused; };
struct ASensor { int type; };
struct ASensorEventQueue { int unused; };

struct BbrInputEvent {
    int32_t type;
    int32_t source;
    int32_t device_id;
    int32_t action;
    int32_t keycode;
    int32_t pointer_id;
    float x;
    float y;
    float axes[32];
};

static android_app *runtime_app;
static ANativeActivity *runtime_activity;
static struct ALooper runtime_looper;
static struct ANativeWindow runtime_window = { 960, 544, 1 };
static struct ASensorManager runtime_sensor_manager;
static struct ASensor runtime_accelerometer = { 1 };
static struct ASensorEventQueue runtime_sensor_queue;
static unsigned command_index;
static const int32_t startup_commands[] = {
    APP_CMD_START, APP_CMD_RESUME, APP_CMD_INIT_WINDOW,
    APP_CMD_GAINED_FOCUS,
};
static float analog_axes[32];

_Static_assert(offsetof(android_app, window) == 36,
               "android_app.window ABI mismatch");
_Static_assert(offsetof(android_app, cmdPollSource) == 84,
               "android_app.cmdPollSource ABI mismatch");
_Static_assert(sizeof(android_app) == 148,
               "android_app size ABI mismatch");

static void process_command(android_app *app, android_poll_source *source) {
    (void)source;
    if (!app || command_index >=
        sizeof(startup_commands) / sizeof(startup_commands[0]))
        return;

    int32_t command = startup_commands[command_index++];
    if (command == APP_CMD_INIT_WINDOW) {
        app->window = &runtime_window;
        app->pendingWindow = &runtime_window;
    }
    if (command == APP_CMD_START || command == APP_CMD_RESUME)
        app->activityState = command;
    l_info("Delivering Android app command %d.", command);
    port_trace("NativeActivity: command=%d window=%p callback=%p", command,
               app->window, app->onAppCmd);
    if (app->onAppCmd)
        app->onAppCmd(app, command);
    port_trace("NativeActivity: command=%d completed", command);
}

static void process_input_queue(android_app *app,
                                android_poll_source *source) {
    (void)app;
    (void)source;
}

android_app *native_activity_create(JavaVM *vm, JNIEnv *env,
                                    AAssetManager *asset_manager,
                                    const char *data_path) {
    android_app *app = calloc(1, sizeof(*app));
    ANativeActivity *activity = calloc(1, sizeof(*activity));
    ANativeActivityCallbacks *callbacks = calloc(1, 128);
    if (!app || !activity || !callbacks)
        return NULL;

    activity->callbacks = callbacks;
    activity->vm = vm;
    activity->env = env;
    activity->clazz = (jobject)activity;
    activity->internalDataPath = strdup(data_path);
    activity->externalDataPath = activity->internalDataPath;
    activity->sdkVersion = 19;
    activity->assetManager = asset_manager;
    activity->obbPath = activity->internalDataPath;

    app->activity = activity;
    app->config = AConfiguration_new();
    app->looper = &runtime_looper;
    /* Android starts without a window. APP_CMD_INIT_WINDOW publishes the
     * pending window immediately before invoking the game's callback. */
    app->window = NULL;
    app->pendingWindow = &runtime_window;
    app->contentRect = (ARect){ 0, 0, 960, 544 };
    app->pendingContentRect = app->contentRect;
    app->cmdPollSource = (android_poll_source){ 1, app, process_command };
    app->inputPollSource = (android_poll_source){
        2, app, process_input_queue
    };
    app->running = 1;

    activity->instance = app;
    runtime_app = app;
    runtime_activity = activity;
    command_index = 0;
    memset(analog_axes, 0, sizeof(analog_axes));
    port_trace("NativeActivity: created app=%p activity=%p pendingWindow=%p",
               app, activity, app->pendingWindow);
    return app;
}

ANativeActivity *native_activity_get_activity(void) {
    return runtime_activity;
}

static void dispatch_input(struct BbrInputEvent *event) {
    if (runtime_app && runtime_app->onInputEvent)
        runtime_app->onInputEvent(runtime_app, event);
}

void native_activity_send_key(int32_t keycode, ControlsAction action) {
    struct BbrInputEvent event = { 0 };
    event.type = AINPUT_EVENT_TYPE_KEY;
    event.source = AINPUT_SOURCE_GAMEPAD;
    event.device_id = 1;
    event.action = action == CONTROLS_ACTION_UP
        ? AKEY_EVENT_ACTION_UP : AKEY_EVENT_ACTION_DOWN;
    event.keycode = keycode;
    dispatch_input(&event);
}

void native_activity_send_touch(int32_t pointer_id, float x, float y,
                                ControlsAction action) {
    struct BbrInputEvent event = { 0 };
    event.type = AINPUT_EVENT_TYPE_MOTION;
    event.source = AINPUT_SOURCE_TOUCHSCREEN;
    event.device_id = 2;
    event.action = action == CONTROLS_ACTION_DOWN
        ? AMOTION_EVENT_ACTION_DOWN
        : action == CONTROLS_ACTION_UP
              ? AMOTION_EVENT_ACTION_UP : AMOTION_EVENT_ACTION_MOVE;
    event.pointer_id = pointer_id;
    event.x = x;
    event.y = y;
    dispatch_input(&event);
}

void native_activity_send_analog(ControlsStickId which, float x, float y,
                                 ControlsAction action) {
    (void)action;
    if (which == CONTROLS_STICK_LEFT) {
        analog_axes[AMOTION_EVENT_AXIS_X] = x;
        analog_axes[AMOTION_EVENT_AXIS_Y] = y;
    } else {
        analog_axes[AMOTION_EVENT_AXIS_Z] = x;
        analog_axes[AMOTION_EVENT_AXIS_RZ] = y;
    }

    struct BbrInputEvent event = { 0 };
    event.type = AINPUT_EVENT_TYPE_MOTION;
    event.source = AINPUT_SOURCE_JOYSTICK;
    event.device_id = 1;
    event.action = AMOTION_EVENT_ACTION_MOVE;
    memcpy(event.axes, analog_axes, sizeof(analog_axes));
    dispatch_input(&event);
}

AConfiguration *AConfiguration_new(void) {
    struct AConfiguration *config = calloc(1, sizeof(*config));
    if (config) {
        config->language[0] = 'e'; config->language[1] = 'n';
        config->country[0] = 'U'; config->country[1] = 'S';
    }
    return config;
}

void AConfiguration_delete(AConfiguration *config) { free(config); }
void AConfiguration_fromAssetManager(AConfiguration *config,
                                     AAssetManager *manager) {
    (void)config; (void)manager;
}
void AConfiguration_getLanguage(AConfiguration *config, char *language) {
    if (language) memcpy(language, config ? config->language : "en", 2);
}
void AConfiguration_getCountry(AConfiguration *config, char *country) {
    if (country) memcpy(country, config ? config->country : "US", 2);
}

ALooper *ALooper_prepare(int opts) { (void)opts; return &runtime_looper; }
int ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                  void *callback, void *data) {
    (void)looper; (void)fd; (void)ident; (void)events;
    (void)callback; (void)data;
    return 1;
}

int ALooper_pollAll(int timeout_millis, int *out_fd, int *out_events,
                    void **out_data) {
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;

    if (runtime_app && command_index <
        sizeof(startup_commands) / sizeof(startup_commands[0])) {
        port_trace("NativeActivity: poll delivers startup index=%u timeout=%d",
                   command_index, timeout_millis);
        if (out_data) {
            *out_data = &runtime_app->cmdPollSource;
        } else {
            process_command(runtime_app, &runtime_app->cmdPollSource);
        }
        return 1;
    }

    if (out_data) *out_data = NULL;
    controls_poll();
    int delay = timeout_millis > 0 ? timeout_millis * 1000 : 1000;
    if (delay > 10000) delay = 10000;
    sceKernelDelayThread(delay);
    return ALOOPER_POLL_TIMEOUT;
}

void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                              void *callback, void *data) {
    (void)queue; (void)looper; (void)ident; (void)callback; (void)data;
}
void AInputQueue_detachLooper(AInputQueue *queue) { (void)queue; }
int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **event) {
    (void)queue; if (event) *event = NULL; return -1;
}
int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event) {
    (void)queue; (void)event; return 0;
}
void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event,
                             int handled) {
    (void)queue; (void)event; (void)handled;
}

int32_t AInputEvent_getType(const AInputEvent *event) { return event->type; }
int32_t AInputEvent_getSource(const AInputEvent *event) {
    return event->source;
}
int32_t AInputEvent_getDeviceId(const AInputEvent *event) {
    return event->device_id;
}
int32_t AKeyEvent_getAction(const AInputEvent *event) { return event->action; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *event) {
    return event->keycode;
}
int32_t AMotionEvent_getAction(const AInputEvent *event) {
    return event->action;
}
size_t AMotionEvent_getPointerCount(const AInputEvent *event) {
    (void)event; return 1;
}
int32_t AMotionEvent_getPointerId(const AInputEvent *event,
                                  size_t pointer_index) {
    (void)pointer_index; return event->pointer_id;
}
float AMotionEvent_getX(const AInputEvent *event, size_t pointer_index) {
    (void)pointer_index; return event->x;
}
float AMotionEvent_getY(const AInputEvent *event, size_t pointer_index) {
    (void)pointer_index; return event->y;
}
float AMotionEvent_getAxisValue(const AInputEvent *event, int32_t axis,
                                size_t pointer_index) {
    (void)pointer_index;
    return axis >= 0 && axis < 32 ? event->axes[axis] : 0.0f;
}

void ANativeActivity_finish(ANativeActivity *activity) {
    (void)activity;
    if (runtime_app) runtime_app->destroyRequested = 1;
}
void ANativeActivity_setWindowFlags(ANativeActivity *activity,
                                    uint32_t add_flags,
                                    uint32_t remove_flags) {
    (void)activity; (void)add_flags; (void)remove_flags;
}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width,
                                         int32_t height, int32_t format) {
    if (window) {
        if (width > 0) window->width = width;
        if (height > 0) window->height = height;
        window->format = format;
    }
    return 0;
}

ASensorManager *ASensorManager_getInstance(void) {
    return &runtime_sensor_manager;
}
const ASensor *ASensorManager_getDefaultSensor(ASensorManager *manager,
                                               int type) {
    (void)manager;
    runtime_accelerometer.type = type;
    return &runtime_accelerometer;
}
ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *manager,
                                                   ALooper *looper, int ident,
                                                   void *callback,
                                                   void *data) {
    (void)manager; (void)looper; (void)ident; (void)callback; (void)data;
    return &runtime_sensor_queue;
}
int ASensorEventQueue_enableSensor(ASensorEventQueue *queue,
                                   const ASensor *sensor) {
    (void)queue; (void)sensor; return 0;
}
int ASensorEventQueue_disableSensor(ASensorEventQueue *queue,
                                    const ASensor *sensor) {
    (void)queue; (void)sensor; return 0;
}
int ASensorEventQueue_setEventRate(ASensorEventQueue *queue,
                                   const ASensor *sensor,
                                   int32_t usec) {
    (void)queue; (void)sensor; (void)usec; return 0;
}
int ASensorEventQueue_getEvents(ASensorEventQueue *queue, void *events,
                                size_t count) {
    (void)queue; (void)events; (void)count; return 0;
}
