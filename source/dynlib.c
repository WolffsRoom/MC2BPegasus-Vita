/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dynlib.c
 * @brief Resolving dynamic imports of the .so.
 */

#include <psp2/kernel/clib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <locale.h>
#include <poll.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <so_util/so_util.h>
#include <utime.h>

#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"
#include "reimpl/native_activity.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <psp2/audioout.h>
#include <pthread.h>

#include "reimpl/errno.h"
#include "reimpl/io.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"
#include "reimpl/egl.h"
#include "reimpl/time64.h"
#include "reimpl/asset_manager.h"

const unsigned int __page_size = PAGE_SIZE;

/* Extra Android/GNU runtime objects imported by Sandstorm 2. */
static uintptr_t mc2_dso_handle;
static const unsigned char mc2_nothrow_object;

static void *mc2_new_array_nothrow(size_t size, const void *nothrow_tag) {
    (void)nothrow_tag;
    return malloc(size);
}

static void mc2_clearerr(FILE *stream) {
    /* Streams are supplied by SceLibcBridge in the default build.  Newlib's
     * clearerr cannot safely inspect that private FILE layout. */
    (void)stream;
}

static uint32_t mc2_inet_addr(const char *text) {
    struct in_addr address;
    return inet_aton(text, &address) ? address.s_addr : 0xffffffffu;
}

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZSt17__throw_bad_allocv;
extern void * _ZSt9terminatev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void * __cxa_allocate_exception;
extern void * __cxa_begin_catch;
extern void * __cxa_end_catch;
extern void * __cxa_free_exception;
extern void * __cxa_rethrow;
extern void * __cxa_throw;
extern void * __gxx_personality_v0;
extern void *_ZNSt8bad_castD1Ev;
extern void *_ZTISt8bad_cast;
extern void *_ZTISt9exception;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_memclr;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_memset;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];

void *dlsym_soloader(void * handle, const char * symbol);
extern void port_trace(const char *format, ...);

static int bbr_setpriority(int which, int who, int priority) {
    (void)which; (void)who; (void)priority;
    return 0;
}

/* FMOD 1.08's Android Reverb3D wrapper occasionally validates a newly
 * created reverb to the sentinel pointer 0x1 on Vita.  Its next attribute
 * copy then writes through 0x4d and aborts the process.  Purple uses this
 * object only for environmental reverb, so keep the rest of FMOD/FMOD Studio
 * native and make these four optional object methods harmless. */
static unsigned bbr_fmod_reverb_call_count;

static void bbr_trace_fmod_reverb(const char *operation, void *self) {
    const unsigned call = ++bbr_fmod_reverb_call_count;
    if (call <= 16)
        port_trace("FMOD Reverb3D bypass #%u operation=%s self=%p",
                   call, operation, self);
}

static int bbr_fmod_reverb_set_3d_attributes(
        void *self, const void *position, float minimum_distance,
        float maximum_distance) {
    (void)position;
    (void)minimum_distance;
    (void)maximum_distance;
    bbr_trace_fmod_reverb("set3DAttributes", self);
    return 0;
}

static int bbr_fmod_reverb_set_properties(void *self,
                                           const void *properties) {
    (void)properties;
    bbr_trace_fmod_reverb("setProperties", self);
    return 0;
}

static int bbr_fmod_reverb_set_active(void *self, int active) {
    (void)active;
    bbr_trace_fmod_reverb("setActive", self);
    return 0;
}

static int bbr_fmod_reverb_release(void *self) {
    bbr_trace_fmod_reverb("release", self);
    return 0;
}

static const struct SLObjectItf_ *opensl_object_original_vtable;
static struct SLObjectItf_ opensl_object_traced_vtable;
static const struct SLEngineItf_ *opensl_engine_original_vtable;
static struct SLEngineItf_ opensl_engine_traced_vtable;

static SLresult opensl_android_config_set(
        SLAndroidConfigurationItf self, const SLchar *config_key,
        const void *config_value, SLuint32 value_size) {
    (void)self;
    (void)config_value;
    port_trace("OpenSL AndroidConfiguration::Set key=%p size=%u (ignored)",
               config_key, value_size);
    return SL_RESULT_SUCCESS;
}

static SLresult opensl_android_config_get(
        SLAndroidConfigurationItf self, const SLchar *config_key,
        SLuint32 *value_size, void *config_value) {
    (void)self;
    port_trace("OpenSL AndroidConfiguration::Get key=%p", config_key);
    if (!value_size)
        return SL_RESULT_PARAMETER_INVALID;

    if (config_value && *value_size >= sizeof(SLint32))
        *(SLint32 *)config_value = 0;
    *value_size = sizeof(SLint32);
    return SL_RESULT_SUCCESS;
}

static const struct SLAndroidConfigurationItf_
    opensl_android_config_vtable = {
        opensl_android_config_set,
        opensl_android_config_get,
    };
static const struct SLAndroidConfigurationItf_ * const
    opensl_android_config_vtable_ptr = &opensl_android_config_vtable;

static const struct SLPlayItf_ *opensl_play_original_vtable;
static struct SLPlayItf_ opensl_play_traced_vtable;
static const struct SLAndroidSimpleBufferQueueItf_
    *opensl_queue_original_vtable;
static struct SLAndroidSimpleBufferQueueItf_ opensl_queue_traced_vtable;
static slAndroidSimpleBufferQueueCallback opensl_queue_client_callback;
static void *opensl_queue_client_context;
static SLAndroidSimpleBufferQueueItf opensl_queue_self;
static uint32_t opensl_enqueue_count;
static uint32_t opensl_callback_count;
static uint32_t opensl_nonzero_count;

static void opensl_queue_callback_traced(
        SLAndroidSimpleBufferQueueItf caller, void *context);

enum {
    FMOD_PCM_RING_CAPACITY = 8,
    FMOD_VITA_OUTPUT_MAX_FRAMES = 2048,
};

typedef struct {
    void *data;
    SLuint32 size;
} fmod_pcm_block;

static fmod_pcm_block fmod_pcm_ring[FMOD_PCM_RING_CAPACITY];
static unsigned fmod_pcm_read;
static unsigned fmod_pcm_write;
static unsigned fmod_pcm_queued;
static SLuint32 fmod_pcm_play_index;
static uint32_t fmod_pcm_generation;
static uint32_t fmod_pcm_source_rate = 24000;
static int fmod_pcm_playing;
static int fmod_pcm_thread_started;
static pthread_t fmod_pcm_thread_handle;
static pthread_mutex_t fmod_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fmod_pcm_cond = PTHREAD_COND_INITIALIZER;
static int16_t fmod_vita_output[2][FMOD_VITA_OUTPUT_MAX_FRAMES * 2]
    __attribute__((aligned(64)));

static void fmod_pcm_clear_locked(void) {
    while (fmod_pcm_queued) {
        fmod_pcm_block *block = &fmod_pcm_ring[fmod_pcm_read];
        free(block->data);
        block->data = NULL;
        block->size = 0;
        fmod_pcm_read = (fmod_pcm_read + 1) % FMOD_PCM_RING_CAPACITY;
        --fmod_pcm_queued;
    }
    fmod_pcm_read = 0;
    fmod_pcm_write = 0;
    ++fmod_pcm_generation;
}

static void *fmod_pcm_output_thread(void *unused) {
    (void)unused;
    int16_t previous_sample[2] = { 0, 0 };
    uint32_t resampler_generation = UINT32_MAX;
    bool previous_sample_valid = false;

    /* Wait for FMOD's first real block so the Vita port length represents
     * exactly the same amount of time at 48 kHz. */
    pthread_mutex_lock(&fmod_pcm_mutex);
    while (!fmod_pcm_playing || !fmod_pcm_queued)
        pthread_cond_wait(&fmod_pcm_cond, &fmod_pcm_mutex);
    unsigned initial_input_frames =
        fmod_pcm_ring[fmod_pcm_read].size / (sizeof(int16_t) * 2);
    uint32_t source_rate = fmod_pcm_source_rate
        ? fmod_pcm_source_rate : 24000;
    pthread_mutex_unlock(&fmod_pcm_mutex);

    unsigned output_frames = (unsigned)(
        ((uint64_t)initial_input_frames * 48000 + source_rate / 2) /
        source_rate);
    output_frames = (output_frames + 63) & ~63u;
    if (output_frames < 64)
        output_frames = 64;
    if (output_frames > FMOD_VITA_OUTPUT_MAX_FRAMES)
        output_frames = FMOD_VITA_OUTPUT_MAX_FRAMES;

    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                   output_frames, 48000,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    port_trace("FMOD direct PCM: MAIN port=%d source_rate=%u input_frames=%u output_frames=%u rate=48000 double_buffer=1",
               port, source_rate, initial_input_frames, output_frames);
    if (port < 0)
        return NULL;

    unsigned output_buffer_index = 0;
    for (;;) {
        pthread_mutex_lock(&fmod_pcm_mutex);
        while (!fmod_pcm_playing || !fmod_pcm_queued)
            pthread_cond_wait(&fmod_pcm_cond, &fmod_pcm_mutex);

        fmod_pcm_block block = fmod_pcm_ring[fmod_pcm_read];
        fmod_pcm_ring[fmod_pcm_read].data = NULL;
        fmod_pcm_ring[fmod_pcm_read].size = 0;
        fmod_pcm_read = (fmod_pcm_read + 1) % FMOD_PCM_RING_CAPACITY;
        --fmod_pcm_queued;
        ++fmod_pcm_play_index;
        uint32_t block_generation = fmod_pcm_generation;
        pthread_cond_broadcast(&fmod_pcm_cond);
        pthread_mutex_unlock(&fmod_pcm_mutex);

        const int16_t *input = (const int16_t *)block.data;
        unsigned input_frames = block.size / (sizeof(int16_t) * 2);
        int16_t *vita_output = fmod_vita_output[output_buffer_index];
        if (input && input_frames) {
            /*
             * FMOD normally supplies 512 stereo frames at 24 kHz.  Convert
             * them to the Vita port's 1024 frames at 48 kHz with linear
             * interpolation.  Keeping the last sample from the preceding
             * block makes interpolation continuous across block boundaries
             * and avoids the clicks and high-frequency images produced by
             * nearest-neighbour sample duplication.
             */
            if (resampler_generation != block_generation) {
                previous_sample_valid = false;
                resampler_generation = block_generation;
            }
            if (!previous_sample_valid) {
                previous_sample[0] = input[0];
                previous_sample[1] = input[1];
                previous_sample_valid = true;
            }

            if (input_frames == output_frames) {
                memcpy(vita_output, input,
                       output_frames * sizeof(int16_t) * 2);
            } else {
                for (unsigned i = 0; i < output_frames; ++i) {
                    uint64_t scaled = (uint64_t)(i + 1) * input_frames;
                    unsigned whole = (unsigned)(scaled / output_frames);
                    unsigned fraction = (unsigned)(scaled % output_frames);
                    unsigned weight_a = output_frames - fraction;

                    for (unsigned channel = 0; channel < 2; ++channel) {
                        int32_t sample_a;
                        int32_t sample_b;
                        if (whole == 0) {
                            sample_a = previous_sample[channel];
                            sample_b = input[channel];
                        } else {
                            unsigned index_a = whole - 1;
                            unsigned index_b = whole < input_frames
                                ? whole : input_frames - 1;
                            sample_a = input[index_a * 2 + channel];
                            sample_b = input[index_b * 2 + channel];
                        }
                        int32_t interpolated =
                            (sample_a * (int32_t)weight_a +
                             sample_b * (int32_t)fraction) /
                            (int32_t)output_frames;
                        vita_output[i * 2 + channel] =
                            (int16_t)interpolated;
                    }
                }
            }

            previous_sample[0] = input[(input_frames - 1) * 2];
            previous_sample[1] = input[(input_frames - 1) * 2 + 1];
        } else {
            memset(vita_output, 0,
                   output_frames * sizeof(int16_t) * 2);
        }

        int output_result = sceAudioOutOutput(port, vita_output);
        output_buffer_index ^= 1;
        free(block.data);
        if (output_result < 0) {
            port_trace("FMOD direct PCM: sceAudioOutOutput failed=0x%x",
                       output_result);
            break;
        }

        if (opensl_queue_client_callback)
            opensl_queue_callback_traced(opensl_queue_self, NULL);
    }

    sceAudioOutReleasePort(port);
    return NULL;
}

static void fmod_pcm_start_thread(void) {
    pthread_mutex_lock(&fmod_pcm_mutex);
    if (fmod_pcm_thread_started) {
        pthread_mutex_unlock(&fmod_pcm_mutex);
        return;
    }
    fmod_pcm_thread_started = 1;
    pthread_mutex_unlock(&fmod_pcm_mutex);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    int result = pthread_create(&fmod_pcm_thread_handle, &attr,
                                fmod_pcm_output_thread, NULL);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        pthread_mutex_lock(&fmod_pcm_mutex);
        fmod_pcm_thread_started = 0;
        pthread_mutex_unlock(&fmod_pcm_mutex);
        port_trace("FMOD direct PCM: pthread_create failed=%d", result);
    }
}

static SLresult opensl_play_set_state_traced(SLPlayItf self,
                                              SLuint32 play_state) {
    SLresult result = opensl_play_original_vtable->SetPlayState(self,
                                                                play_state);
    pthread_mutex_lock(&fmod_pcm_mutex);
    fmod_pcm_playing = play_state == SL_PLAYSTATE_PLAYING;
    if (play_state == SL_PLAYSTATE_STOPPED)
        fmod_pcm_clear_locked();
    pthread_cond_broadcast(&fmod_pcm_cond);
    pthread_mutex_unlock(&fmod_pcm_mutex);
    if (play_state == SL_PLAYSTATE_PLAYING)
        fmod_pcm_start_thread();
    port_trace("OpenSL Play::SetPlayState state=%u result=%u", play_state,
               result);
    return result;
}

static SLresult opensl_queue_enqueue_traced(
        SLAndroidSimpleBufferQueueItf self, const void *buffer,
        SLuint32 size) {
    uint32_t call = __atomic_fetch_add(&opensl_enqueue_count, 1,
                                       __ATOMIC_RELAXED);
    int peak = 0;
    if (buffer) {
        const int16_t *samples = (const int16_t *)buffer;
        SLuint32 sample_count = size / sizeof(int16_t);
        for (SLuint32 i = 0; i < sample_count; ++i) {
            int value = samples[i];
            if (value < 0)
                value = -value;
            if (value > peak)
                peak = value;
        }
    }

    SLresult result = SL_RESULT_SUCCESS;
    void *copy = NULL;
    if (!buffer || !size) {
        result = SL_RESULT_PARAMETER_INVALID;
    } else if (!(copy = malloc(size))) {
        result = SL_RESULT_MEMORY_FAILURE;
    } else {
        memcpy(copy, buffer, size);
        pthread_mutex_lock(&fmod_pcm_mutex);
        if (fmod_pcm_queued >= FMOD_PCM_RING_CAPACITY) {
            result = SL_RESULT_BUFFER_INSUFFICIENT;
        } else {
            fmod_pcm_ring[fmod_pcm_write].data = copy;
            fmod_pcm_ring[fmod_pcm_write].size = size;
            fmod_pcm_write = (fmod_pcm_write + 1) % FMOD_PCM_RING_CAPACITY;
            ++fmod_pcm_queued;
            copy = NULL;
            pthread_cond_broadcast(&fmod_pcm_cond);
        }
        pthread_mutex_unlock(&fmod_pcm_mutex);
        free(copy);
    }
    if (call < 12)
        port_trace("FMOD direct Queue::Enqueue #%u size=%u peak=%d result=%u",
                   call + 1, size, peak, result);
    else if (peak > 0) {
        uint32_t nonzero = __atomic_fetch_add(&opensl_nonzero_count, 1,
                                              __ATOMIC_RELAXED);
        if (nonzero < 16)
            port_trace("FMOD direct nonzero #%u enqueue=%u peak=%d size=%u",
                       nonzero + 1, call + 1, peak, size);
    }
    if (result != SL_RESULT_SUCCESS)
        port_trace("FMOD direct Queue::Enqueue failure #%u result=%u",
                   call + 1, result);
    return result;
}

static SLresult opensl_queue_clear_traced(
        SLAndroidSimpleBufferQueueItf self) {
    (void)self;
    pthread_mutex_lock(&fmod_pcm_mutex);
    fmod_pcm_clear_locked();
    pthread_cond_broadcast(&fmod_pcm_cond);
    pthread_mutex_unlock(&fmod_pcm_mutex);
    port_trace("FMOD direct Queue::Clear");
    return SL_RESULT_SUCCESS;
}

static SLresult opensl_queue_get_state_traced(
        SLAndroidSimpleBufferQueueItf self,
        SLAndroidSimpleBufferQueueState *state) {
    (void)self;
    if (!state)
        return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&fmod_pcm_mutex);
    state->count = fmod_pcm_queued;
    state->index = fmod_pcm_play_index;
    pthread_mutex_unlock(&fmod_pcm_mutex);
    return SL_RESULT_SUCCESS;
}

static void opensl_queue_callback_traced(
        SLAndroidSimpleBufferQueueItf caller, void *context) {
    (void)context;
    uint32_t call = __atomic_fetch_add(&opensl_callback_count, 1,
                                       __ATOMIC_RELAXED);
    if (call < 12)
        port_trace("OpenSL Queue callback #%u caller=%p", call + 1, caller);
    if (opensl_queue_client_callback)
        opensl_queue_client_callback(caller, opensl_queue_client_context);
}

static SLresult opensl_queue_register_callback_traced(
        SLAndroidSimpleBufferQueueItf self,
        slAndroidSimpleBufferQueueCallback callback, void *context) {
    opensl_queue_client_callback = callback;
    opensl_queue_client_context = context;
    opensl_queue_self = self;
    SLresult result = SL_RESULT_SUCCESS;
    port_trace("OpenSL Queue::RegisterCallback callback=%p context=%p result=%u",
               callback, context, result);
    return result;
}

static void trace_opensl_play(SLPlayItf play) {
    if (!play || !*play || *play == &opensl_play_traced_vtable)
        return;
    if (!opensl_play_original_vtable) {
        opensl_play_original_vtable = *play;
        opensl_play_traced_vtable = **play;
        opensl_play_traced_vtable.SetPlayState =
            opensl_play_set_state_traced;
    }
    *(const struct SLPlayItf_ **)play = &opensl_play_traced_vtable;
}

static void trace_opensl_queue(SLAndroidSimpleBufferQueueItf queue) {
    if (!queue || !*queue || *queue == &opensl_queue_traced_vtable)
        return;
    if (!opensl_queue_original_vtable) {
        opensl_queue_original_vtable = *queue;
        opensl_queue_traced_vtable = **queue;
        opensl_queue_traced_vtable.Enqueue = opensl_queue_enqueue_traced;
        opensl_queue_traced_vtable.Clear = opensl_queue_clear_traced;
        opensl_queue_traced_vtable.GetState = opensl_queue_get_state_traced;
        opensl_queue_traced_vtable.RegisterCallback =
            opensl_queue_register_callback_traced;
    }
    *(const struct SLAndroidSimpleBufferQueueItf_ **)queue =
        &opensl_queue_traced_vtable;
}

static void trace_opensl_object(SLObjectItf object);
static void trace_opensl_engine(SLEngineItf engine);

static SLresult opensl_object_realize_traced(SLObjectItf self,
                                              SLboolean async) {
    port_trace("OpenSL Object::Realize enter self=%p async=%u", self, async);
    SLresult result = opensl_object_original_vtable->Realize(self, async);
    port_trace("OpenSL Object::Realize leave self=%p result=%u", self, result);
    return result;
}

static SLresult opensl_object_get_interface_traced(SLObjectItf self,
                                                    const SLInterfaceID iid,
                                                    void *interface_out) {
    port_trace("OpenSL Object::GetInterface enter self=%p iid=%p", self, iid);

    /*
     * The Vita OpenSLES port intentionally omits AndroidConfiguration. FMOD
     * only uses it to select an Android stream category, which has no Vita
     * equivalent, so expose a successful no-op interface instead.
     */
    if (iid == SL_IID_ANDROIDCONFIGURATION && interface_out) {
        *(SLAndroidConfigurationItf *)interface_out =
            &opensl_android_config_vtable_ptr;
        port_trace("OpenSL Object::GetInterface AndroidConfiguration shim out=%p",
                   *(void **)interface_out);
        return SL_RESULT_SUCCESS;
    }

    SLresult result = opensl_object_original_vtable->GetInterface(
        self, iid, interface_out);
    port_trace("OpenSL Object::GetInterface leave self=%p iid=%p result=%u out=%p",
               self, iid, result,
               interface_out ? *(void **)interface_out : NULL);

    if (result == SL_RESULT_SUCCESS && iid == SL_IID_ENGINE && interface_out)
        trace_opensl_engine(*(SLEngineItf *)interface_out);
    else if (result == SL_RESULT_SUCCESS && iid == SL_IID_PLAY && interface_out)
        trace_opensl_play(*(SLPlayItf *)interface_out);
    else if (result == SL_RESULT_SUCCESS &&
             iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE && interface_out)
        trace_opensl_queue(*(SLAndroidSimpleBufferQueueItf *)interface_out);

    return result;
}

static SLresult opensl_engine_create_output_mix_traced(
        SLEngineItf self, SLObjectItf *mix, SLuint32 num_interfaces,
        const SLInterfaceID *interface_ids,
        const SLboolean *interface_required) {
    port_trace("OpenSL Engine::CreateOutputMix enter self=%p interfaces=%u",
               self, num_interfaces);
    SLresult result = opensl_engine_original_vtable->CreateOutputMix(
        self, mix, num_interfaces, interface_ids, interface_required);
    port_trace("OpenSL Engine::CreateOutputMix leave result=%u mix=%p", result,
               mix ? (void *)*mix : NULL);
    if (result == SL_RESULT_SUCCESS && mix)
        trace_opensl_object(*mix);
    return result;
}

static SLresult opensl_engine_create_audio_player_traced(
        SLEngineItf self, SLObjectItf *player, SLDataSource *audio_source,
        SLDataSink *audio_sink, SLuint32 num_interfaces,
        const SLInterfaceID *interface_ids,
        const SLboolean *interface_required) {
    port_trace("OpenSL Engine::CreateAudioPlayer enter self=%p interfaces=%u",
               self, num_interfaces);

    if (audio_source && audio_source->pFormat) {
        const SLDataFormat_PCM *pcm =
            (const SLDataFormat_PCM *)audio_source->pFormat;
        if (pcm->formatType == SL_DATAFORMAT_PCM) {
            if (pcm->samplesPerSec >= 1000)
                fmod_pcm_source_rate = pcm->samplesPerSec / 1000;
            port_trace("OpenSL player PCM channels=%u rate_mHz=%u bits=%u container=%u mask=0x%x",
                       pcm->numChannels, pcm->samplesPerSec,
                       pcm->bitsPerSample, pcm->containerSize,
                       pcm->channelMask);
        }
    }

    /*
     * AndroidConfiguration is compiled out of the Vita OpenSLES class table.
     * Requiring it makes CreateAudioPlayer fail with FEATURE_UNSUPPORTED before
     * the supported PCM buffer queue can be constructed. Filter that one IID;
     * GetInterface serves the harmless shim above when FMOD configures it.
     */
    enum { OPENSL_MAX_PLAYER_INTERFACES = 16 };
    SLInterfaceID filtered_ids[OPENSL_MAX_PLAYER_INTERFACES];
    SLboolean filtered_required[OPENSL_MAX_PLAYER_INTERFACES];
    SLuint32 filtered_count = 0;

    if (num_interfaces <= OPENSL_MAX_PLAYER_INTERFACES) {
        for (SLuint32 i = 0; i < num_interfaces; ++i) {
            if (interface_ids[i] == SL_IID_ANDROIDCONFIGURATION) {
                port_trace("OpenSL CreateAudioPlayer filtering AndroidConfiguration required=%u",
                           interface_required ? interface_required[i] : 0);
                continue;
            }
            filtered_ids[filtered_count] = interface_ids[i];
            filtered_required[filtered_count] =
                interface_required ? interface_required[i] : SL_BOOLEAN_FALSE;
            ++filtered_count;
        }
    } else {
        filtered_count = num_interfaces;
    }

    SLresult result = opensl_engine_original_vtable->CreateAudioPlayer(
        self, player, audio_source, audio_sink, filtered_count,
        filtered_count <= OPENSL_MAX_PLAYER_INTERFACES ? filtered_ids
                                                       : interface_ids,
        filtered_count <= OPENSL_MAX_PLAYER_INTERFACES ? filtered_required
                                                       : interface_required);
    port_trace("OpenSL Engine::CreateAudioPlayer leave result=%u player=%p",
               result, player ? (void *)*player : NULL);
    if (result == SL_RESULT_SUCCESS && player)
        trace_opensl_object(*player);
    return result;
}

static void trace_opensl_object(SLObjectItf object) {
    if (!object || !*object || *object == &opensl_object_traced_vtable)
        return;

    if (!opensl_object_original_vtable) {
        opensl_object_original_vtable = *object;
        opensl_object_traced_vtable = **object;
        opensl_object_traced_vtable.Realize = opensl_object_realize_traced;
        opensl_object_traced_vtable.GetInterface =
            opensl_object_get_interface_traced;
    }

    *(const struct SLObjectItf_ **)object = &opensl_object_traced_vtable;
}

static void trace_opensl_engine(SLEngineItf engine) {
    if (!engine || !*engine || *engine == &opensl_engine_traced_vtable)
        return;

    if (!opensl_engine_original_vtable) {
        opensl_engine_original_vtable = *engine;
        opensl_engine_traced_vtable = **engine;
        opensl_engine_traced_vtable.CreateOutputMix =
            opensl_engine_create_output_mix_traced;
        opensl_engine_traced_vtable.CreateAudioPlayer =
            opensl_engine_create_audio_player_traced;
    }

    *(const struct SLEngineItf_ **)engine = &opensl_engine_traced_vtable;
}

static SLresult slCreateEngine_traced(SLObjectItf *engine,
                                      SLuint32 num_options,
                                      const SLEngineOption *options,
                                      SLuint32 num_interfaces,
                                      const SLInterfaceID *interface_ids,
                                      const SLboolean *interface_required) {
    SLresult result = slCreateEngine(engine, num_options, options,
                                     num_interfaces, interface_ids,
                                     interface_required);
    port_trace("slCreateEngine result=%u engine=%p", result,
               engine ? (void *)*engine : NULL);
    if (result == SL_RESULT_SUCCESS && engine)
        trace_opensl_object(*engine);
    return result;
}

so_default_dynlib default_dynlib[] = {
        // BBR: bypass the broken optional FMOD Reverb3D object path.
        { "_ZN4FMOD8Reverb3D15set3DAttributesEPK11FMOD_VECTORff", (uintptr_t)&bbr_fmod_reverb_set_3d_attributes },
        { "_ZN4FMOD8Reverb3D13setPropertiesEPK22FMOD_REVERB_PROPERTIES", (uintptr_t)&bbr_fmod_reverb_set_properties },
        { "_ZN4FMOD8Reverb3D9setActiveEb", (uintptr_t)&bbr_fmod_reverb_set_active },
        { "_ZN4FMOD8Reverb3D7releaseEv", (uintptr_t)&bbr_fmod_reverb_release },

        // Common C/C++ internals
        { "__dso_handle", (uintptr_t)&mc2_dso_handle },
        { "_ZSt7nothrow", (uintptr_t)&mc2_nothrow_object },
        { "_ZnajRKSt9nothrow_t", (uintptr_t)&mc2_new_array_nothrow },
        { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev },
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
        { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
        { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast },
        { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
        { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
        { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
        { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&_Znaj },
        { "_Znwj", (uintptr_t)&_Znwj },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
        { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
        { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
        { "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
        { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
        { "__aeabi_f2lz", (uintptr_t)&__aeabi_f2lz },
        { "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_memclr", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr4", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr8", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memmove", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove4", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove8", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memset", (uintptr_t)&__aeabi_memset },
        { "__aeabi_memset4",  (uintptr_t)&__aeabi_memset4 },
        { "__aeabi_memset8", (uintptr_t)&__aeabi_memset8 },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
        { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
        { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
        { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
        { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
        { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
        { "__cxa_throw", (uintptr_t)&__cxa_throw },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
        { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
        { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
        { "__isinf", (uintptr_t)&ret0 },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        { "__assert2", (uintptr_t)&ret0 }, // TODO: stub/impl
        { "dl_unwind_find_exidx", (uintptr_t)&ret0 }, // TODO: stub/impl


        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalnum", (uintptr_t)&isalnum },
        { "isalpha", (uintptr_t)&isalpha },
        { "isblank", (uintptr_t)&isblank },
        { "iscntrl", (uintptr_t)&iscntrl },
        { "isgraph", (uintptr_t)&isgraph },
        { "islower", (uintptr_t)&islower },
        { "isprint", (uintptr_t)&isprint },
        { "ispunct", (uintptr_t)&ispunct },
        { "isspace", (uintptr_t)&isspace },
        { "isupper", (uintptr_t)&isupper },
        { "isxdigit", (uintptr_t)&isxdigit },
        { "tolower", (uintptr_t)&tolower },
        { "toupper", (uintptr_t)&toupper },


        // Android SDK standard logging
        { "__android_log_assert", (uintptr_t)&__android_log_assert },
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
        { "__android_log_write", (uintptr_t)&__android_log_write },


        // AAssetManager
        { "AAsset_close", (uintptr_t)&AAsset_close },
        { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
        { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
        { "AAsset_read", (uintptr_t)&AAsset_read },
        { "AAsset_seek", (uintptr_t)&AAsset_seek },
        { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
        { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
        { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
        { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_soloader },
        { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
        { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },


        // Android NativeActivity, looper, input and sensor bridge
        { "AConfiguration_delete", (uintptr_t)&AConfiguration_delete },
        { "AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager },
        { "AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry },
        { "AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage },
        { "AConfiguration_new", (uintptr_t)&AConfiguration_new },
        { "AInputEvent_getDeviceId", (uintptr_t)&AInputEvent_getDeviceId },
        { "AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource },
        { "AInputEvent_getType", (uintptr_t)&AInputEvent_getType },
        { "AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper },
        { "AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper },
        { "AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent },
        { "AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent },
        { "AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent },
        { "AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction },
        { "AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode },
        { "ALooper_addFd", (uintptr_t)&ALooper_addFd },
        { "ALooper_pollAll", (uintptr_t)&ALooper_pollAll },
        { "ALooper_prepare", (uintptr_t)&ALooper_prepare },
        { "AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction },
        { "AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue },
        { "AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount },
        { "AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId },
        { "AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX },
        { "AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY },
        { "ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish },
        { "ANativeActivity_setWindowFlags", (uintptr_t)&ANativeActivity_setWindowFlags },
        { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },
        { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor },
        { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor },
        { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents },
        { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate },
        { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue },
        { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor },
        { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance },


        // Math
        { "acos", (uintptr_t)&acos },
        { "acosf", (uintptr_t)&acosf },
        { "asin", (uintptr_t)&asin },
        { "asinf", (uintptr_t)&asinf },
        { "atan", (uintptr_t)&atan },
        { "atan2", (uintptr_t)&atan2 },
        { "atan2f", (uintptr_t)&atan2f },
        { "atanf", (uintptr_t)&atanf },
        { "ceil", (uintptr_t)&ceil },
        { "ceilf", (uintptr_t)&ceilf },
        { "cos", (uintptr_t)&cos },
        { "cosf", (uintptr_t)&cosf },
        { "exp", (uintptr_t)&exp },
        { "exp2", (uintptr_t)&exp2 },
        { "exp2f", (uintptr_t)&exp2f },
        { "expf", (uintptr_t)&expf },
        { "floor", (uintptr_t)&floor },
        { "floorf", (uintptr_t)&floorf },
        { "fmax", (uintptr_t)&fmax },
        { "fmaxf", (uintptr_t)&fmaxf },
        { "fmin", (uintptr_t)&fmin },
        { "fminf", (uintptr_t)&fminf },
        { "frexpf", (uintptr_t)&frexpf },
        { "fmod", (uintptr_t)&fmod },
        { "fmodf", (uintptr_t)&fmodf },
        { "frexp", (uintptr_t)&frexp },
        { "ldexp", (uintptr_t)&ldexp },
        { "ldexpf", (uintptr_t)&ldexpf },
        { "log", (uintptr_t)&log },
        { "log10", (uintptr_t)&log10 },
        { "log10f", (uintptr_t)&log10f },
        { "logf", (uintptr_t)&logf },
        { "lrint", (uintptr_t)&lrint },
        { "lrintf", (uintptr_t)&lrintf },
        { "lround", (uintptr_t)&lround },
        { "lroundf", (uintptr_t)&lroundf },
        { "modf", (uintptr_t)&modf },
        { "pow", (uintptr_t)&pow },
        { "powf", (uintptr_t)&powf },
        { "rint", (uintptr_t)&rint },
        { "rintf", (uintptr_t)&rintf },
        { "round", (uintptr_t)&round },
        { "roundf", (uintptr_t)&roundf },
        { "scalbn", (uintptr_t)&scalbn },
        { "scalbnf", (uintptr_t)&scalbnf },
        { "sin", (uintptr_t)&sin },
        { "sincos", (uintptr_t)&sincos },
        { "sincosf", (uintptr_t)&sincosf },
        { "sinf", (uintptr_t)&sinf },
        { "sinh", (uintptr_t)&sinh },
        { "sinhf", (uintptr_t)&sinhf },
        { "sqrt", (uintptr_t)&sqrt },
        { "sqrtf", (uintptr_t)&sqrtf },
        { "tan", (uintptr_t)&tan },
        { "tanf", (uintptr_t)&tanf },
        { "tanh", (uintptr_t)&tanh },
        { "trunc", (uintptr_t)&trunc },
        { "truncf", (uintptr_t)&truncf },


        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "gai_strerror", (uintptr_t)&ret0 },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostbyaddr", (uintptr_t)&gethostbyaddr },
        { "gethostbyname", (uintptr_t)&gethostbyname },
        { "gethostname", (uintptr_t)&gethostname },
        { "getpeername", (uintptr_t)&getpeername },
        { "getservbyname", (uintptr_t)&getservbyname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "inet_aton", (uintptr_t)&inet_aton },
        { "inet_addr", (uintptr_t)&mc2_inet_addr },
        { "inet_pton", (uintptr_t)&inet_pton },
        { "inet_ntoa", (uintptr_t)&inet_ntoa },
        { "inet_ntop", (uintptr_t)&inet_ntop },
        { "listen", (uintptr_t)&listen },
        { "poll", (uintptr_t)&poll },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "recvmsg", (uintptr_t)&recvmsg },
        { "select", (uintptr_t)&select },
        { "setpriority", (uintptr_t)&bbr_setpriority },
        { "send", (uintptr_t)&send },
        { "sendmsg", (uintptr_t)&sendmsg },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "shutdown", (uintptr_t)&shutdown },
        { "socket", (uintptr_t)&socket },


        // Memory
        { "calloc", (uintptr_t)&calloc },
        { "free", (uintptr_t)&free },
        { "malloc", (uintptr_t)&malloc },
        { "memalign", (uintptr_t)&memalign },
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmem", (uintptr_t)&memmem },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "mmap", (uintptr_t)&mmap },
        { "__mmap2", (uintptr_t)&mmap },
        { "munmap", (uintptr_t)&munmap },
        { "realloc", (uintptr_t)&realloc },
        { "valloc", (uintptr_t)&valloc },


        // IO
        { "close", (uintptr_t)&close_soloader },
        { "closedir", (uintptr_t)&closedir_soloader },
        { "execv", (uintptr_t)&ret0 },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "clearerr", (uintptr_t)&mc2_clearerr },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "fsync", (uintptr_t)&fsync_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "__open_2", (uintptr_t)&open_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "readdir_r", (uintptr_t)&readdir_r_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },

        #ifdef USE_SCELIBC_IO
            { "fdopen", (uintptr_t)&sceLibcBridge_fdopen },
            { "feof", (uintptr_t)&sceLibcBridge_feof },
            { "ferror", (uintptr_t)&sceLibcBridge_ferror },
            { "fflush", (uintptr_t)&sceLibcBridge_fflush },
            { "fgetc", (uintptr_t)&sceLibcBridge_fgetc },
            { "fgetpos", (uintptr_t)&sceLibcBridge_fgetpos },
            { "fgets", (uintptr_t)&sceLibcBridge_fgets },
            { "fileno", (uintptr_t)&sceLibcBridge_fileno },
            { "fputc", (uintptr_t)&sceLibcBridge_fputc },
            { "fputs", (uintptr_t)&sceLibcBridge_fputs },
            { "fread", (uintptr_t)&sceLibcBridge_fread },
            { "freopen", (uintptr_t)&sceLibcBridge_freopen },
            { "fseek", (uintptr_t)&sceLibcBridge_fseek },
            { "fsetpos", (uintptr_t)&sceLibcBridge_fsetpos },
            { "ftell", (uintptr_t)&sceLibcBridge_ftell },
            { "fwide", (uintptr_t)&sceLibcBridge_fwide },
            { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
            { "getc", (uintptr_t)&sceLibcBridge_getc },
            { "getwc", (uintptr_t)&sceLibcBridge_getwc },
            { "putc", (uintptr_t)&sceLibcBridge_putc },
            { "putchar", (uintptr_t)&sceLibcBridge_putchar },
            { "puts", (uintptr_t)&sceLibcBridge_puts },
            { "putwc", (uintptr_t)&sceLibcBridge_putwc },
            { "setvbuf", (uintptr_t)&sceLibcBridge_setvbuf },
            { "ungetc", (uintptr_t)&sceLibcBridge_ungetc },
            { "ungetwc", (uintptr_t)&sceLibcBridge_ungetwc },
        #else
            { "fdopen", (uintptr_t)&fdopen },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fileno", (uintptr_t)&fileno },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread },
            { "freopen", (uintptr_t)&freopen },
            { "fseek", (uintptr_t)&fseek },
            { "fsetpos", (uintptr_t)&fsetpos },
            { "ftell", (uintptr_t)&ftell },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "getc", (uintptr_t)&getc },
            { "getwc", (uintptr_t)&getwc },
            { "putc", (uintptr_t)&putc },
            { "putchar", (uintptr_t)&putchar },
            { "puts", (uintptr_t)&puts },
            { "putwc", (uintptr_t)&putwc },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        #endif

        { "access", (uintptr_t)&access_soloader },
        { "basename", (uintptr_t)&basename },
        { "chdir", (uintptr_t)&chdir_soloader },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup },
        { "fseeko", (uintptr_t)&fseeko }, // TODO: wrap normal fseek for SceLibc version?
        { "ftello", (uintptr_t)&ftello },
        { "ftruncate", (uintptr_t)&ftruncate },
        { "getcwd", (uintptr_t)&getcwd },
        { "lseek", (uintptr_t)&lseek },
        { "lseek64", (uintptr_t)&ret0 }, // TODO: implement or stub with warning
        { "lstat", (uintptr_t)&lstat_soloader },
        { "mkdir", (uintptr_t)&mkdir_soloader },
        { "pipe", (uintptr_t)&pipe },
        { "read", (uintptr_t)&read },
        { "realpath", (uintptr_t)&realpath },
        { "remove", (uintptr_t)&remove_soloader },
        { "rename", (uintptr_t)&rename_soloader },
        { "rewind", (uintptr_t)&rewind },
        { "rmdir", (uintptr_t)&rmdir },
        { "truncate", (uintptr_t)&truncate },
        { "unlink", (uintptr_t)&unlink },
        { "write", (uintptr_t)&write },


        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf },
        { "sprintf", (uintptr_t)&sprintf },
        { "vasprintf", (uintptr_t)&vasprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf },
        { "vsprintf", (uintptr_t)&vsprintf },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
        { "printf", (uintptr_t)&sceClibPrintf },
        { "swprintf", (uintptr_t)&swprintf },

        #ifdef USE_SCELIBC_IO
            { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
            { "fscanf", (uintptr_t)&sceLibcBridge_fscanf },
            { "sscanf", (uintptr_t)&sceLibcBridge_sscanf },
            { "vfprintf", (uintptr_t)&sceLibcBridge_vfprintf },
        #else
            { "fprintf", (uintptr_t)&fprintf },
            { "fscanf", (uintptr_t)&fscanf },
            { "sscanf", (uintptr_t)&sscanf },
            { "vfprintf", (uintptr_t)&vfprintf },
        #endif


        // EGL
        { "eglBindAPI", (uintptr_t)&eglBindAPI },
        { "eglChooseConfig", (uintptr_t)&bbr_eglChooseConfig },
        { "eglCreateContext", (uintptr_t)&bbr_eglCreateContext },
        { "eglCreatePbufferSurface", (uintptr_t)&bbr_eglCreatePbufferSurface },
        { "eglCreateWindowSurface", (uintptr_t)&bbr_eglCreateWindowSurface },
        { "eglDestroyContext", (uintptr_t)&bbr_eglDestroyContext },
        { "eglDestroySurface", (uintptr_t)&bbr_eglDestroySurface },
        { "eglGetConfigAttrib", (uintptr_t)&bbr_eglGetConfigAttrib },
        { "eglGetConfigs", (uintptr_t)&bbr_eglGetConfigs },
        { "eglGetCurrentContext", (uintptr_t)&bbr_eglGetCurrentContext },
        { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
        { "eglGetError", (uintptr_t)&eglGetError },
        { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
        { "eglInitialize", (uintptr_t)&bbr_eglInitialize },
        { "eglMakeCurrent", (uintptr_t)&bbr_eglMakeCurrent },
        { "eglQueryContext", (uintptr_t)&bbr_eglQueryContext },
        { "eglQueryString", (uintptr_t)&bbr_eglQueryString },
        { "eglQuerySurface", (uintptr_t)&bbr_eglQuerySurface },
        { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers },
        { "eglTerminate", (uintptr_t)&bbr_eglTerminate },


        // OpenGL
        { "glActiveTexture", (uintptr_t)&glActiveTexture },
        { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
        { "glAlphaFuncx", (uintptr_t)&glAlphaFuncx },
        { "glAttachShader", (uintptr_t)&glAttachShader_soloader },
        { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation_soloader },
        { "glBindBuffer", (uintptr_t)&glBindBuffer },
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
        { "glBindFramebufferOES", (uintptr_t)&glBindFramebuffer },
        { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
        { "glBindRenderbufferOES", (uintptr_t)&glBindRenderbuffer },
        { "glBindTexture", (uintptr_t)&glBindTexture },
        { "glBlendColor", (uintptr_t)&glBlendColor_soloader },
        { "glBlendEquation", (uintptr_t)&glBlendEquation },
        { "glBlendEquationOES", (uintptr_t)&glBlendEquation },
        { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendFunc", (uintptr_t)&glBlendFunc_soloader },
        { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate_soloader },
        { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate_soloader },
        { "glBufferData", (uintptr_t)&glBufferData },
        { "glBufferSubData", (uintptr_t)&glBufferSubData },
        { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus_soloader },
        { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus_soloader },
        { "glClear", (uintptr_t)&glClear },
        { "glClearColor", (uintptr_t)&glClearColor },
        { "glClearColorx", (uintptr_t)&glClearColorx },
        { "glClearDepthf", (uintptr_t)&glClearDepthf },
        { "glClearDepthx", (uintptr_t)&glClearDepthx },
        { "glClearStencil", (uintptr_t)&glClearStencil },
        { "glClientActiveTexture", (uintptr_t)&glClientActiveTexture },
        { "glClipPlanef", (uintptr_t)&glClipPlanef },
        { "glClipPlanex", (uintptr_t)&glClipPlanex },
        { "glColor4f", (uintptr_t)&glColor4f },
        { "glColor4ub", (uintptr_t)&glColor4ub },
        { "glColor4x", (uintptr_t)&glColor4x },
        { "glColorMask", (uintptr_t)&glColorMask },
        { "glColorPointer", (uintptr_t)&glColorPointer },
        { "glCompileShader", (uintptr_t)&glCompileShader_soloader },
        { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_soloader },
        { "glCompressedTexImage3DOES", (uintptr_t)&ret0 },
        { "glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D_soloader },
        { "glCompressedTexSubImage3DOES", (uintptr_t)&ret0 },
        { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
        { "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D },
        { "glCreateProgram", (uintptr_t)&glCreateProgram_soloader },
        { "glCreateShader", (uintptr_t)&glCreateShader_soloader },
        { "glCullFace", (uintptr_t)&glCullFace },
        { "glCurrentPaletteMatrixOES", (uintptr_t)&ret0 },
        { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
        { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteProgram", (uintptr_t)&glDeleteProgram_soloader },
        { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteRenderbuffersOES", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteShader", (uintptr_t)&glDeleteShader_soloader },
        { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
        { "glDepthFunc", (uintptr_t)&glDepthFunc },
        { "glDepthMask", (uintptr_t)&glDepthMask },
        { "glDepthRangef", (uintptr_t)&glDepthRangef },
        { "glDepthRangex", (uintptr_t)&glDepthRangex },
        { "glDetachShader", (uintptr_t)&ret0 },
        { "glDisable", (uintptr_t)&glDisable },
        { "glDisableClientState", (uintptr_t)&glDisableClientState },
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
        { "glDrawArrays", (uintptr_t)&glDrawArrays },
        { "glDrawElements", (uintptr_t)&glDrawElements },
        { "glDrawTexfOES", (uintptr_t)&ret0 },
        { "glDrawTexfvOES", (uintptr_t)&ret0 },
        { "glDrawTexiOES", (uintptr_t)&ret0 },
        { "glDrawTexivOES", (uintptr_t)&ret0 },
        { "glDrawTexsOES", (uintptr_t)&ret0 },
        { "glDrawTexsvOES", (uintptr_t)&ret0 },
        { "glDrawTexxOES", (uintptr_t)&ret0 },
        { "glDrawTexxvOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetRenderbufferStorageOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetTexture2DOES", (uintptr_t)&ret0 },
        { "glEnable", (uintptr_t)&glEnable },
        { "glEnableClientState", (uintptr_t)&glEnableClientState },
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
        { "glFinish", (uintptr_t)&glFinish },
        { "glFlush", (uintptr_t)&glFlush },
        { "glFogf", (uintptr_t)&glFogf },
        { "glFogfv", (uintptr_t)&glFogfv },
        { "glFogx", (uintptr_t)&glFogx },
        { "glFogxv", (uintptr_t)&glFogxv },
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferRenderbufferOES", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
        { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D },
        { "glFrontFace", (uintptr_t)&glFrontFace },
        { "glFrustumf", (uintptr_t)&glFrustumf },
        { "glFrustumx", (uintptr_t)&glFrustumx },
        { "glGenBuffers", (uintptr_t)&glGenBuffers },
        { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
        { "glGenerateMipmapOES", (uintptr_t)&glGenerateMipmap },
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
        { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
        { "glGenRenderbuffersOES", (uintptr_t)&glGenRenderbuffers },
        { "glGenTextures", (uintptr_t)&glGenTextures },
        { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
        { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
        { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
        { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameteriv },
        { "glGetBufferPointervOES", (uintptr_t)&ret0 },
        { "glGetClipPlanef", (uintptr_t)&ret0 },
        { "glGetClipPlanex", (uintptr_t)&ret0 },
        { "glGetError", (uintptr_t)&glGetError_soloader },
        { "glGetFixedv", (uintptr_t)&ret0 },
        { "glGetFloatv", (uintptr_t)&glGetFloatv },
        { "glGetFramebufferAttachmentParameterivOES", (uintptr_t)&glGetFramebufferAttachmentParameteriv },
        { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
        { "glGetLightfv", (uintptr_t)&ret0 },
        { "glGetLightxv", (uintptr_t)&ret0 },
        { "glGetMaterialfv", (uintptr_t)&ret0 },
        { "glGetMaterialxv", (uintptr_t)&ret0 },
        { "glGetPointerv", (uintptr_t)&ret0 },
        { "glGetRenderbufferParameterivOES", (uintptr_t)&ret0 },
        { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
        { "glGetProgramiv", (uintptr_t)&glGetProgramiv_soloader },
        { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
        { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
        { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
        { "glGetString", (uintptr_t)&glGetString_soloader },
        { "glGetTexEnvfv", (uintptr_t)&ret0 },
        { "glGetTexEnviv", (uintptr_t)&glGetTexEnviv },
        { "glGetTexEnvxv", (uintptr_t)&ret0 },
        { "glGetTexGenfvOES", (uintptr_t)&ret0 },
        { "glGetTexGenivOES", (uintptr_t)&ret0 },
        { "glGetTexGenxvOES", (uintptr_t)&ret0 },
        { "glGetTexParameterfv", (uintptr_t)&ret0 },
        { "glGetTexParameteriv", (uintptr_t)&ret0 },
        { "glGetTexParameterxv", (uintptr_t)&ret0 },
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
        { "glHint", (uintptr_t)&glHint },
        { "glIsBuffer", (uintptr_t)&ret0 },
        { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
        { "glIsEnabled", (uintptr_t)&glIsEnabled },
        { "glIsFramebufferOES", (uintptr_t)&glIsFramebuffer },
        { "glIsRenderbufferOES", (uintptr_t)&glIsRenderbuffer },
        { "glIsTexture", (uintptr_t)&glIsTexture },
        { "glLightf", (uintptr_t)&ret0 },
        { "glLightfv", (uintptr_t)&glLightfv },
        { "glLightModelf", (uintptr_t)&ret0 },
        { "glLightModelfv", (uintptr_t)&glLightModelfv },
        { "glLightModelx", (uintptr_t)&ret0 },
        { "glLightModelxv", (uintptr_t)&glLightModelxv },
        { "glLightx", (uintptr_t)&ret0 },
        { "glLightxv", (uintptr_t)&glLightxv },
        { "glLineWidth", (uintptr_t)&glLineWidth },
        { "glLineWidthx", (uintptr_t)&glLineWidthx },
        { "glLinkProgram", (uintptr_t)&glLinkProgram_soloader },
        { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
        { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
        { "glLoadMatrixx", (uintptr_t)&glLoadMatrixx },
        { "glLoadPaletteFromModelViewMatrixOES", (uintptr_t)&ret0 },
        { "glLogicOp", (uintptr_t)&ret0 },
        { "glMapBuffer", (uintptr_t)&glMapBuffer },
        { "glMapBufferOES", (uintptr_t)&glMapBuffer },
        { "glMaterialf", (uintptr_t)&glMaterialf },
        { "glMaterialfv", (uintptr_t)&glMaterialfv },
        { "glMaterialx", (uintptr_t)&glMaterialx },
        { "glMaterialxv", (uintptr_t)&glMaterialxv },
        { "glMatrixIndexPointerOES", (uintptr_t)&ret0 },
        { "glMatrixMode", (uintptr_t)&glMatrixMode },
        { "glMultiTexCoord4f", (uintptr_t)&ret0 },
        { "glMultiTexCoord4x", (uintptr_t)&ret0},
        { "glMultMatrixf", (uintptr_t)&glMultMatrixf },
        { "glMultMatrixx", (uintptr_t)&glMultMatrixx },
        { "glNormal3f", (uintptr_t)&glNormal3f },
        { "glNormal3x", (uintptr_t)&glNormal3x },
        { "glNormalPointer", (uintptr_t)&glNormalPointer },
        { "glOrthof", (uintptr_t)&glOrthof },
        { "glOrthox", (uintptr_t)&glOrthox },
        { "glPixelStorei", (uintptr_t)&glPixelStorei },
        { "glPointParameterf", (uintptr_t)&ret0 },
        { "glPointParameterfv", (uintptr_t)&ret0 },
        { "glPointParameterx", (uintptr_t)&ret0 },
        { "glPointParameterxv", (uintptr_t)&ret0 },
        { "glPointSize", (uintptr_t)&glPointSize },
        { "glPointSizePointerOES", (uintptr_t)&ret0 },
        { "glPointSizex", (uintptr_t)&glPointSizex },
        { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
        { "glPolygonOffsetx", (uintptr_t)&glPolygonOffsetx },
        { "glPopMatrix", (uintptr_t)&glPopMatrix },
        { "glPushMatrix", (uintptr_t)&glPushMatrix },
        { "glQueryMatrixxOES", (uintptr_t)&ret0 },
        { "glReadPixels", (uintptr_t)&glReadPixels },
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
        { "glRenderbufferStorageOES", (uintptr_t)&glRenderbufferStorage },
        { "glRotatef", (uintptr_t)&glRotatef },
        { "glRotatex", (uintptr_t)&glRotatex },
        { "glSampleCoverage", (uintptr_t)&ret0 },
        { "glSampleCoveragex", (uintptr_t)&ret0 },
        { "glScalef", (uintptr_t)&glScalef },
        { "glScalex", (uintptr_t)&glScalex },
        { "glScissor", (uintptr_t)&glScissor },
        { "glShadeModel", (uintptr_t)&glShadeModel },
        { "glShaderSource", (uintptr_t)&glShaderSource_soloader },
        { "glStencilFunc", (uintptr_t)&glStencilFunc },
        { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
        { "glStencilMask", (uintptr_t)&glStencilMask },
        { "glStencilOp", (uintptr_t)&glStencilOp },
        { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
        { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
        { "glTexEnvf", (uintptr_t)&glTexEnvf },
        { "glTexEnvfv", (uintptr_t)&glTexEnvfv },
        { "glTexEnvi", (uintptr_t)&glTexEnvi },
        { "glTexEnviv", (uintptr_t)&ret0 },
        { "glTexEnvx", (uintptr_t)&glTexEnvx },
        { "glTexEnvxv", (uintptr_t)&glTexEnvxv },
        { "glTexGenfOES", (uintptr_t)&ret0 },
        { "glTexGenfvOES", (uintptr_t)&ret0 },
        { "glTexGeniOES", (uintptr_t)&ret0 },
        { "glTexGenivOES", (uintptr_t)&ret0 },
        { "glTexGenxOES", (uintptr_t)&ret0 },
        { "glTexGenxvOES", (uintptr_t)&ret0 },
        { "glTexImage2D", (uintptr_t)&glTexImage2D },
        { "glTexImage3DOES", (uintptr_t)&ret0 },
        { "glTexParameterf", (uintptr_t)&glTexParameterf_soloader },
        { "glTexParameterfv", (uintptr_t)&ret0 },
        { "glTexParameteri", (uintptr_t)&glTexParameteri_soloader },
        { "glTexParameteriv", (uintptr_t)&glTexParameteriv_soloader },
        { "glTexParameterx", (uintptr_t)&glTexParameterx_soloader },
        { "glTexParameterxv", (uintptr_t)&ret0 },
        { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
        { "glTexSubImage3DOES", (uintptr_t)&ret0 },
        { "glTranslatef", (uintptr_t)&glTranslatef },
        { "glTranslatex", (uintptr_t)&glTranslatex },
        { "glUniform1f", (uintptr_t)&glUniform1f },
        { "glUniform1fv", (uintptr_t)&glUniform1fv },
        { "glUniform1i", (uintptr_t)&glUniform1i },
        { "glUniform1iv", (uintptr_t)&glUniform1iv },
        { "glUniform2i", (uintptr_t)&glUniform2i },
        { "glUniform2f", (uintptr_t)&glUniform2f },
        { "glUniform2fv", (uintptr_t)&glUniform2fv },
        { "glUniform2iv", (uintptr_t)&glUniform2iv },
        { "glUniform3i", (uintptr_t)&glUniform3i },
        { "glUniform3f", (uintptr_t)&glUniform3f },
        { "glUniform3fv", (uintptr_t)&glUniform3fv },
        { "glUniform3iv", (uintptr_t)&glUniform3iv },
        { "glUniform4i", (uintptr_t)&glUniform4i },
        { "glUniform4f", (uintptr_t)&glUniform4f },
        { "glUniform4fv", (uintptr_t)&glUniform4fv },
        { "glUniform4iv", (uintptr_t)&glUniform4iv },
        { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
        { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
        { "glUnmapBuffer", (uintptr_t)&glUnmapBuffer },
        { "glUnmapBufferOES", (uintptr_t)&glUnmapBuffer },
        { "glUseProgram", (uintptr_t)&glUseProgram },
        { "glValidateProgram", (uintptr_t)&ret0 },
        { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f },
        { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
        { "glVertexPointer", (uintptr_t)&glVertexPointer },
        { "glViewport", (uintptr_t)&glViewport },
        { "glWeightPointerOES", (uintptr_t)&ret0 },


        // OpenSLES
        { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
        { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },
        { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
        { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
        { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION },
        { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
        { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS },
        { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
        { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
        { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
        { "slCreateEngine", (uintptr_t)&slCreateEngine_traced },


        // Pthread
        { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_attr_setschedparam", (uintptr_t) &ret0 },

        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },

        { "pthread_create", (uintptr_t) &pthread_create_soloader },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_equal", (uintptr_t) &pthread_equal_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
        { "pthread_kill", (uintptr_t)&pthread_kill_soloader },

        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_mutexattr_setpshared", (uintptr_t) &ret0 },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },

        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setname_np", (uintptr_t) &pthread_setname_np_soloader },
        { "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
        { "pthread_sigmask", (uintptr_t)&ret0 },

        { "sem_destroy", (uintptr_t) &sem_destroy_soloader },
        { "sem_getvalue", (uintptr_t) &sem_getvalue_soloader },
        { "sem_init", (uintptr_t) &sem_init_soloader },
        { "sem_post", (uintptr_t) &sem_post_soloader },
        { "sem_timedwait", (uintptr_t) &sem_timedwait_soloader },
        { "sem_trywait", (uintptr_t) &sem_trywait_soloader },
        { "sem_wait", (uintptr_t) &sem_wait_soloader },

        { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
        { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
        { "sched_yield", (uintptr_t)&sched_yield },


        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswcntrl", (uintptr_t)&iswcntrl },
        { "iswctype", (uintptr_t)&iswctype },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswlower", (uintptr_t)&iswlower },
        { "iswprint", (uintptr_t)&iswprint },
        { "iswpunct", (uintptr_t)&iswpunct },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "mbstowcs", (uintptr_t)&mbstowcs },
        { "mbtowc", (uintptr_t)&mbtowc },
        { "towlower", (uintptr_t)&towlower },
        { "towupper", (uintptr_t)&towupper },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscasecmp", (uintptr_t)&wcscasecmp },
        { "wcscmp", (uintptr_t)&wcscmp },
        { "wcscoll", (uintptr_t)&wcscoll },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcsftime", (uintptr_t)&wcsftime },
        { "wcslcat", (uintptr_t)&wcslcat },
        { "wcslcpy", (uintptr_t)&wcslcpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
        { "wcsncmp", (uintptr_t)&wcsncmp },
        { "wcsncpy", (uintptr_t)&wcsncpy },
        { "wcsnlen", (uintptr_t)&wcsnlen },
        { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
        { "wcsstr", (uintptr_t)&wcsstr },
        { "wcstod", (uintptr_t)&wcstod },
        { "wcstof", (uintptr_t)&wcstof },
        { "wcstol", (uintptr_t)&wcstol },
        { "wcstoll", (uintptr_t)&wcstoll },
        { "wcstombs", (uintptr_t)&wcstombs },
        { "wcstoul", (uintptr_t)&wcstoul },
        { "wcstoull", (uintptr_t)&wcstoull },
        { "wcsxfrm", (uintptr_t)&wcsxfrm },
        { "wctob", (uintptr_t)&wctob },
        { "wctype", (uintptr_t)&wctype },
        { "wmemchr", (uintptr_t)&wmemchr },
        { "wmemcmp", (uintptr_t)&wmemcmp },
        { "wmemcpy", (uintptr_t)&wmemcpy },
        { "wmemmove", (uintptr_t)&wmemmove },
        { "wmemset", (uintptr_t)&wmemset },


        // libdl
        { "dlclose", (uintptr_t)&ret0 },
        { "dlerror", (uintptr_t)&ret0 },
        { "dlopen", (uintptr_t)&ret1 },
        { "dlsym", (uintptr_t)&dlsym_soloader },


        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        { "strerror_r", (uintptr_t)&strerror_r_soloader },
        { "perror", (uintptr_t)&perror }, // TODO: errno translation


        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "memrchr", (uintptr_t)&memrchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strcspn", (uintptr_t)&strcspn },
        { "strdup", (uintptr_t)&strdup },
        { "strlcat", (uintptr_t)&strlcat },
        { "strlcpy", (uintptr_t)&strlcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncasecmp", (uintptr_t)&strncasecmp },
        { "strncat", (uintptr_t)&strncat },
        { "strncmp", (uintptr_t)&strncmp },
        { "strncpy", (uintptr_t)&strncpy },
        { "strnlen", (uintptr_t)&strnlen },
        { "strpbrk", (uintptr_t)&strpbrk },
        { "strrchr", (uintptr_t)&strrchr },
        { "strspn", (uintptr_t)&strspn },
        { "strstr", (uintptr_t)&strstr },
        { "strtok", (uintptr_t)&strtok },
        { "strtok_r", (uintptr_t)&strtok_r },
        { "strxfrm", (uintptr_t)&strxfrm },


        // Syscalls
        { "fork", (uintptr_t)&fork },
        { "getpagesize", (uintptr_t)&getpagesize },
        { "getpid", (uintptr_t)&getpid },
        { "sbrk", (uintptr_t)&sbrk },
        { "syscall", (uintptr_t)&syscall },
        { "sysconf", (uintptr_t)&ret0 },
        { "system", (uintptr_t)&system },
        { "waitpid", (uintptr_t)&ret0 },


        // Time
        { "clock", (uintptr_t)&clock_soloader },
        { "clock_getres", (uintptr_t)&clock_getres_soloader },
        { "clock_gettime", (uintptr_t)&clock_gettime_soloader },
        { "difftime", (uintptr_t)&difftime },
        { "gettimeofday", (uintptr_t)&gettimeofday },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime64", (uintptr_t)&gmtime64 },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime64", (uintptr_t)&localtime64 },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "mktime64", (uintptr_t)&mktime64 },
        { "nanosleep", (uintptr_t)&nanosleep },
        { "strftime", (uintptr_t)&strftime },
        { "time", (uintptr_t)&time },
        { "tzset", (uintptr_t)&tzset },


        // Temp
        { "mkstemp", (uintptr_t)&mkstemp },
        { "mktemp", (uintptr_t)&mktemp },
        { "tmpfile", (uintptr_t)&tmpfile },
        { "tmpnam", (uintptr_t)&tmpnam },


        // stdlib
        { "abort", (uintptr_t)&abort_soloader },
        { "atof", (uintptr_t)&atof },
        { "atoi", (uintptr_t)&atoi },
        { "atol", (uintptr_t)&atol },
        { "atoll", (uintptr_t)&atoll },
        { "bsearch", (uintptr_t)&bsearch },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "prctl", (uintptr_t)&ret0 },
        { "sleep", (uintptr_t)&sleep },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod },
        { "strtof", (uintptr_t)&strtof },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtol", (uintptr_t)&strtol },
        { "strtold", (uintptr_t)&strtold },
        { "strtoll", (uintptr_t)&strtoll },
        { "strtoul", (uintptr_t)&strtoul },
        { "strtoull", (uintptr_t)&strtoull },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep },

        #ifdef USE_SCELIBC_IO
            { "qsort", (uintptr_t)&sceLibcBridge_qsort },
            { "rand", (uintptr_t)&sceLibcBridge_rand },
            { "srand", (uintptr_t)&sceLibcBridge_srand },
        #else
            { "qsort", (uintptr_t)&qsort },
            { "rand", (uintptr_t)&rand },
            { "srand", (uintptr_t)&srand },
        #endif


        // Env
        { "getenv", (uintptr_t)&getenv_soloader },
        { "setenv", (uintptr_t)&setenv_soloader },


        // Jmp
        { "setjmp", (uintptr_t)&setjmp }, // TODO: May have different struct size?
        { "longjmp", (uintptr_t)&longjmp }, // TODO: May have different struct size?


        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },


        // Locale
        { "freelocale", (uintptr_t)&freelocale },
        { "localeconv", (uintptr_t)&localeconv },
        { "newlocale", (uintptr_t)&newlocale },
        { "setlocale", (uintptr_t)&setlocale },
        { "uselocale", (uintptr_t)&uselocale },


        // zlib
        { "adler32", (uintptr_t)&adler32 },
        { "compress", (uintptr_t)&compress },
        { "compressBound", (uintptr_t)&compressBound },
        { "crc32", (uintptr_t)&crc32 },
        { "deflate", (uintptr_t)&deflate },
        { "deflateEnd", (uintptr_t)&deflateEnd },
        { "deflateInit2_", (uintptr_t)&deflateInit2_ },
        { "deflateInit_", (uintptr_t)&deflateInit_ },
        { "deflateReset", (uintptr_t)&deflateReset },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "inflateInit_", (uintptr_t)&inflateInit_ },
        { "inflateReset", (uintptr_t)&inflateReset },
        { "inflateReset2", (uintptr_t)&inflateReset2 },
        { "uncompress", (uintptr_t)&uncompress },
};

void *dlsym_soloader(void * handle, const char * symbol) {
    (void)handle;

    for (int i = 0; i < sizeof(default_dynlib) / sizeof(default_dynlib[0]); i++) {
        if (strcmp(symbol, default_dynlib[i].symbol) == 0) {
            void *address = (void *)default_dynlib[i].func;
            port_trace("dlsym resolved: %s -> %p", symbol, address);
            return address;
        }
    }

    port_trace("dlsym missing: %s", symbol);
    l_error("dlsym: Unknown symbol \"%s\".", symbol);
    return NULL;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;

    so_resolve(mod, default_dynlib, sizeof(default_dynlib), 0);
}
