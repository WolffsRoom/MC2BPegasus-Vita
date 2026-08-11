#ifndef BBR_NATIVE_ACTIVITY_H
#define BBR_NATIVE_ACTIVITY_H

#include <stddef.h>
#include <stdint.h>

#include <falso_jni/FalsoJNI.h>

#include "reimpl/asset_manager.h"
#include "reimpl/controls.h"

typedef struct ALooper ALooper;
typedef struct AInputQueue AInputQueue;
typedef struct ANativeWindow ANativeWindow;
typedef struct AConfiguration AConfiguration;
typedef struct ASensorManager ASensorManager;
typedef struct ASensor ASensor;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct BbrInputEvent AInputEvent;

typedef struct ARect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} ARect;

typedef struct ANativeActivityCallbacks ANativeActivityCallbacks;

typedef struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    JavaVM *vm;
    JNIEnv *env;
    jobject clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;
    AAssetManager *assetManager;
    const char *obbPath;
} ANativeActivity;

struct android_app;

typedef struct android_poll_source {
    int32_t id;
    struct android_app *app;
    void (*process)(struct android_app *app,
                    struct android_poll_source *source);
} android_poll_source;

/*
 * ABI layout from android_native_app_glue.h for 32-bit Bionic.  Bionic's
 * pthread mutex, condition and pthread_t fields are four-byte values in the
 * Android binary, so they are represented as raw words here rather than Vita
 * pthread types.
 */
typedef struct android_app {
    void *userData;
    void (*onAppCmd)(struct android_app *app, int32_t cmd);
    int32_t (*onInputEvent)(struct android_app *app, AInputEvent *event);
    ANativeActivity *activity;
    AConfiguration *config;
    void *savedState;
    size_t savedStateSize;
    ALooper *looper;
    AInputQueue *inputQueue;
    ANativeWindow *window;
    ARect contentRect;
    int32_t activityState;
    int32_t destroyRequested;
    uint32_t mutex;
    uint32_t cond;
    int32_t msgread;
    int32_t msgwrite;
    uint32_t thread;
    android_poll_source cmdPollSource;
    android_poll_source inputPollSource;
    int32_t running;
    int32_t stateSaved;
    int32_t destroyed;
    int32_t redrawNeeded;
    AInputQueue *pendingInputQueue;
    ANativeWindow *pendingWindow;
    ARect pendingContentRect;
} android_app;

android_app *native_activity_create(JavaVM *vm, JNIEnv *env,
                                    AAssetManager *asset_manager,
                                    const char *data_path);
ANativeActivity *native_activity_get_activity(void);

void native_activity_send_key(int32_t keycode, ControlsAction action);
void native_activity_send_touch(int32_t pointer_id, float x, float y,
                                ControlsAction action);
void native_activity_send_analog(ControlsStickId which, float x, float y,
                                 ControlsAction action);

/* Android NDK functions imported by libPurple.so. */
AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(AConfiguration *config);
void AConfiguration_fromAssetManager(AConfiguration *config,
                                     AAssetManager *manager);
void AConfiguration_getLanguage(AConfiguration *config, char *language);
void AConfiguration_getCountry(AConfiguration *config, char *country);

ALooper *ALooper_prepare(int opts);
int ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                  void *callback, void *data);
int ALooper_pollAll(int timeout_millis, int *out_fd, int *out_events,
                    void **out_data);

void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                              void *callback, void *data);
void AInputQueue_detachLooper(AInputQueue *queue);
int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **event);
int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event);
void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event,
                             int handled);

int32_t AInputEvent_getType(const AInputEvent *event);
int32_t AInputEvent_getSource(const AInputEvent *event);
int32_t AInputEvent_getDeviceId(const AInputEvent *event);
int32_t AKeyEvent_getAction(const AInputEvent *event);
int32_t AKeyEvent_getKeyCode(const AInputEvent *event);
int32_t AMotionEvent_getAction(const AInputEvent *event);
size_t AMotionEvent_getPointerCount(const AInputEvent *event);
int32_t AMotionEvent_getPointerId(const AInputEvent *event,
                                  size_t pointer_index);
float AMotionEvent_getX(const AInputEvent *event, size_t pointer_index);
float AMotionEvent_getY(const AInputEvent *event, size_t pointer_index);
float AMotionEvent_getAxisValue(const AInputEvent *event, int32_t axis,
                                size_t pointer_index);

void ANativeActivity_finish(ANativeActivity *activity);
void ANativeActivity_setWindowFlags(ANativeActivity *activity,
                                    uint32_t add_flags,
                                    uint32_t remove_flags);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width,
                                         int32_t height, int32_t format);

ASensorManager *ASensorManager_getInstance(void);
const ASensor *ASensorManager_getDefaultSensor(ASensorManager *manager,
                                               int type);
ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *manager,
                                                   ALooper *looper, int ident,
                                                   void *callback,
                                                   void *data);
int ASensorEventQueue_enableSensor(ASensorEventQueue *queue,
                                   const ASensor *sensor);
int ASensorEventQueue_disableSensor(ASensorEventQueue *queue,
                                    const ASensor *sensor);
int ASensorEventQueue_setEventRate(ASensorEventQueue *queue,
                                   const ASensor *sensor,
                                   int32_t usec);
int ASensorEventQueue_getEvents(ASensorEventQueue *queue, void *events,
                                size_t count);

#endif
