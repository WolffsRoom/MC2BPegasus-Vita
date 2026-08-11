/* Modern Combat 2 / Gameloft Sandstorm 2 trace and patches. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <psp2/io/fcntl.h>

#include <kubridge.h>
#include <so_util/so_util.h>

extern so_module so_mod;
void port_trace(const char *format, ...);

static pthread_mutex_t port_trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static SceUID port_trace_fd = -1;
static char port_trace_buffer[16 * 1024];
static size_t port_trace_used;
static so_hook sound_manager_stop_hook;
static unsigned rejected_sound_stops;

/*
 * The second mission can ask SoundManagerVox to stop the sentinel file id
 * -1 while a cinematic sound is being replaced.  The Android timing normally
 * prevents that path, but on Vita the asynchronous decoder makes it reachable.
 * The original ARM routine passes FileManager::_GetName(-1)'s integer sentinel
 * to strncmp as a pointer and aborts.  Reject only that invalid request and let
 * every valid stop continue through the original function.
 */
static int sound_manager_stop_safe(void *self, int file_id, int instance_id) {
    if (file_id < 0) {
        if (rejected_sound_stops++ < 16)
            port_trace("AUDIO: ignored native SoundManagerVox::Stop "
                       "file_id=%d instance_id=%d", file_id, instance_id);
        return 0;
    }
    return SO_CONTINUE(int, sound_manager_stop_hook,
                       self, file_id, instance_id);
}

static void port_trace_flush_locked(void) {
    if (!port_trace_used)
        return;
    if (port_trace_fd < 0)
        port_trace_fd = sceIoOpen(DATA_PATH "port.log",
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                                  0666);
    if (port_trace_fd < 0) {
        port_trace_used = 0;
        return;
    }
    int written = sceIoWrite(port_trace_fd, port_trace_buffer,
                             port_trace_used);
    if (written <= 0)
        return;
    if ((size_t)written < port_trace_used) {
        memmove(port_trace_buffer, port_trace_buffer + written,
                port_trace_used - (size_t)written);
        port_trace_used -= (size_t)written;
    } else {
        port_trace_used = 0;
    }
}

void port_trace_flush(void) {
    pthread_mutex_lock(&port_trace_mutex);
    port_trace_flush_locked();
    pthread_mutex_unlock(&port_trace_mutex);
}

void port_trace(const char *format, ...) {
    char line[768];
    pthread_mutex_lock(&port_trace_mutex);
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (length < 0) {
        pthread_mutex_unlock(&port_trace_mutex);
        return;
    }
    if (length > (int)sizeof(line) - 2)
        length = sizeof(line) - 2;
    line[length++] = '\n';
    if (port_trace_used + (size_t)length > sizeof(port_trace_buffer))
        port_trace_flush_locked();
    if ((size_t)length <= sizeof(port_trace_buffer) - port_trace_used) {
        memcpy(port_trace_buffer + port_trace_used, line, (size_t)length);
        port_trace_used += (size_t)length;
    }
    pthread_mutex_unlock(&port_trace_mutex);
}

void so_patch(void) {
    port_trace("patch: MC2 Vita v24 reached so_patch");
    port_trace("libsandstorm2=%p", (void *)so_mod.text_base);

    uintptr_t sound_stop = so_symbol(
        &so_mod, "_ZN15SoundManagerVox4StopEii");
    if (sound_stop) {
        sound_manager_stop_hook = hook_addr(
            sound_stop, (uintptr_t)&sound_manager_stop_safe);
        port_trace("patch: guarded SoundManagerVox::Stop at %p",
                   (void *)sound_stop);
    } else {
        port_trace("patch: WARNING SoundManagerVox::Stop symbol not found");
    }
}
