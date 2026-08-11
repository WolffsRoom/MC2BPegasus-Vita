/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define PTHR_INITIAL_OBJECT_CAPACITY 128

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

static void **initialized_objects;
static size_t initialized_object_count;
static size_t initialized_object_capacity;
static pthread_mutex_t initialized_objects_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lazy_init_lock = PTHREAD_MUTEX_INITIALIZER;

int isObjectInitialized(const void * mut) {
    pthread_mutex_lock(&initialized_objects_lock);
    for (size_t i = 0; i < initialized_object_count; ++i) {
        if (initialized_objects[i] == mut) {
            pthread_mutex_unlock(&initialized_objects_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&initialized_objects_lock);
    return 0;
}

int rememberObject(void * mut) {
    pthread_mutex_lock(&initialized_objects_lock);

    if (initialized_object_count == initialized_object_capacity) {
        size_t new_capacity = initialized_object_capacity
            ? initialized_object_capacity * 2
            : PTHR_INITIAL_OBJECT_CAPACITY;
        if (new_capacity < initialized_object_capacity ||
            new_capacity > SIZE_MAX / sizeof(*initialized_objects)) {
            pthread_mutex_unlock(&initialized_objects_lock);
            return 0;
        }

        void **new_objects = realloc(initialized_objects,
                                     new_capacity * sizeof(*new_objects));
        if (!new_objects) {
            pthread_mutex_unlock(&initialized_objects_lock);
            return 0;
        }
        initialized_objects = new_objects;
        initialized_object_capacity = new_capacity;
    }

    initialized_objects[initialized_object_count++] = mut;
    pthread_mutex_unlock(&initialized_objects_lock);
    return 1;
}

int forgetObject(const void * mut) {
    pthread_mutex_lock(&initialized_objects_lock);
    for (size_t i = 0; i < initialized_object_count; ++i) {
        if (initialized_objects[i] == mut) {
            initialized_objects[i] = initialized_objects[--initialized_object_count];
            pthread_mutex_unlock(&initialized_objects_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&initialized_objects_lock);
    return 0;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    int ret = 0;
    pthread_mutex_lock(&lazy_init_lock);
    if (attr->magic != 0x42424242) {
        pthread_attr_t *real_attr = malloc(sizeof(*real_attr));
        if (!real_attr) {
            pthread_mutex_unlock(&lazy_init_lock);
            return ENOMEM;
        }
        ret = pthread_attr_init(real_attr);
        if (ret == 0) {
            attr->real_ptr = real_attr;
            attr->magic = 0x42424242;
        } else {
            free(real_attr);
        }
    }
    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    pthread_mutex_lock(&lazy_init_lock);
    if (isObjectInitialized(mutex)) {
        pthread_mutex_unlock(&lazy_init_lock);
        return ret;
    }

    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t *real_mutex = malloc(sizeof(*real_mutex));
    if (!real_mutex) {
        pthread_mutex_unlock(&lazy_init_lock);
        return ENOMEM;
    }

    pthread_mutexattr_t mutattr;
    ret = pthread_mutexattr_init(&mutattr);
    int mutattr_initialized = (ret == 0);
    if (ret == 0)
        ret = pthread_mutexattr_settype(&mutattr, kind);
    if (ret == 0)
        ret = pthread_mutex_init(real_mutex, &mutattr);
    if (mutattr_initialized)
        pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        if (rememberObject(mutex)) {
            mutex->real_ptr = real_mutex;
        } else {
            pthread_mutex_destroy(real_mutex);
            free(real_mutex);
            ret = ENOMEM;
        }
    } else {
        free(real_mutex);
        l_error("mutex initialization for %p has failed", mutex);
    }

    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    pthread_mutex_lock(&lazy_init_lock);
    if (isObjectInitialized(cond)) {
        pthread_mutex_unlock(&lazy_init_lock);
        return ret;
    }

    pthread_cond_t *real_cond = malloc(sizeof(*real_cond));
    if (!real_cond) {
        pthread_mutex_unlock(&lazy_init_lock);
        return ENOMEM;
    }

    ret = pthread_cond_init(real_cond, attr);

    if (ret == 0) {
        if (rememberObject(cond)) {
            cond->real_ptr = real_cond;
        } else {
            pthread_cond_destroy(real_cond);
            free(real_cond);
            ret = ENOMEM;
        }
    } else {
        free(real_cond);
        l_error("cond initialization for %p has failed", cond);
    }

    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        ret = _attr_t_static_init((pthread_attr_t_bionic *) attr);
        if (ret != 0)
            return ret;
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    pthread_mutex_lock(&lazy_init_lock);
    if (!isObjectInitialized(mutex)) {
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }
    forgetObject(mutex);
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    int ret = _mutex_t_static_init(mutex, NULL);
    if (ret != 0) return ret;
    return pthread_mutex_lock(mutex->real_ptr);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    int ret = _mutex_t_static_init(mutex, NULL);
    if (ret != 0) return ret;
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!isObjectInitialized(mutex)) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    pthread_mutex_lock(&lazy_init_lock);
    if (!isObjectInitialized(cond)) {
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }
    forgetObject(cond);
    int ret = pthread_cond_destroy(cond->real_ptr);
    if (cond->real_ptr) free(cond->real_ptr);
    cond->real_ptr = 0x0;
    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret != 0) return ret;

    return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret != 0) return ret;
    ret = _mutex_t_static_init(mutex, NULL);
    if (ret != 0) return ret;

    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret != 0) return ret;
    ret = _mutex_t_static_init(mutex, NULL);
    if (ret != 0) return ret;

    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    int ret = _cond_t_static_init(cond, NULL);
    if (ret != 0) return ret;

    return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    pthread_mutex_lock(&lazy_init_lock);
    if (attr->magic != 0x42424242) {
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->real_ptr = NULL;
    attr->magic = 0x0;
    pthread_mutex_unlock(&lazy_init_lock);

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    int ret = _attr_t_static_init(attr);
    if (ret != 0) return ret;
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    int ret = _attr_t_static_init(attr);
    if (ret != 0) return ret;
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return EINVAL;

    int expected = 0;
    if (__atomic_compare_exchange_n(once_control, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        (*init_routine)();
        __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
            sceKernelDelayThread(1000);
    }
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    if (!uid || !sval) return -1;
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout - now <= 0) return -1;
    unsigned long long remaining = (unsigned long long) (_timeout - now);
    uint timeout_real = remaining > UINT32_MAX ? UINT32_MAX : (uint) remaining;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
