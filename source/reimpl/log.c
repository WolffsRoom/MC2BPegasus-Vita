/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/log.h"
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdlib.h>

extern void port_trace(const char *format, ...);

/* Sandstorm logs every archive lookup at INFO priority.  Keeping all of that
 * in a file was useful during bring-up, but it caused tens of thousands of
 * synchronous writes while a level was loading.  Preserve startup diagnostics,
 * real warnings/errors and sparse samples without stalling gameplay. */
static void emit_android_log(int prio, const char *tag, const char *text) {
    static unsigned low_priority_count;
    const char *safe_tag = tag ? tag : "";
    const char *safe_text = text ? text : "";

    if (prio < ANDROID_LOG_WARN) {
        unsigned current = ++low_priority_count;
        if (current > 64 && current % 8192 != 0)
            return;
    }

    switch (prio) {
        case ANDROID_LOG_INFO:
            l_info("[ALOG][%s] %s", safe_tag, safe_text);
            break;
        case ANDROID_LOG_WARN:
            l_warn("[ALOG][%s] %s", safe_tag, safe_text);
            break;
        case ANDROID_LOG_ERROR:
        case ANDROID_LOG_FATAL:
            l_error("[ALOG][%s] %s", safe_tag, safe_text);
            break;
        case ANDROID_LOG_UNKNOWN:
        case ANDROID_LOG_DEFAULT:
        case ANDROID_LOG_VERBOSE:
        case ANDROID_LOG_DEBUG:
        case ANDROID_LOG_SILENT:
        default:
            l_debug("[ALOG][%s] %s", safe_tag, safe_text);
            break;
    }
    port_trace("ALOG/%d [%s] %s", prio, safe_tag, safe_text);
}

int __android_log_write(int prio, const char* tag, const char* text) {
    emit_android_log(prio, tag, text);
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list list;
    char text[1024];

    va_start(list, fmt);
    sceClibVsnprintf(text, sizeof(text), fmt, list);
    va_end(list);

    emit_android_log(prio, tag, text);

    return 0;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char text[1024];

    sceClibVsnprintf(text, sizeof(text), fmt, ap);

    emit_android_log(prio, tag, text);

    return 0;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    if (fmt) {
        va_list list;
        char text[1024];

        va_start(list, fmt);
        sceClibVsnprintf(text, sizeof(text), fmt, list);
        va_end(list);

        l_fatal("[ALOG][ASSERT] %s", text);
    } else {
        if (cond) {
            l_fatal("[ALOG][ASSERT] Assertion failed: %s", cond);
        } else {
            l_fatal("[ALOG][ASSERT] Unspecified assertion failed");
        }
    }

    abort();
}
