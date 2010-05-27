/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cutils/atomic.h>
#include <cutils/atomic-inline.h>
#ifdef HAVE_WIN32_THREADS
#include <windows.h>
#else
#include <sched.h>
#endif

/*****************************************************************************/
#if defined(HAVE_MACOSX_IPC)

#include <libkern/OSAtomic.h>

int32_t android_atomic_acquire_load(volatile int32_t* addr) {
    int32_t value = *addr;
    OSMemoryBarrier();
    return value;
}

int32_t android_atomic_release_load(volatile int32_t* addr) {
    OSMemoryBarrier();
    return *addr;
}

void android_atomic_acquire_store(int32_t value, volatile int32_t* addr) {
    *addr = value;
    OSMemoryBarrier();
}

void android_atomic_release_store(int32_t value, volatile int32_t* addr) {
    OSMemoryBarrier();
    *addr = value;
}

int32_t android_atomic_inc(volatile int32_t* addr) {
    return OSAtomicIncrement32Barrier((int32_t*)addr)-1;
}

int32_t android_atomic_dec(volatile int32_t* addr) {
    return OSAtomicDecrement32Barrier((int32_t*)addr)+1;
}

int32_t android_atomic_add(int32_t value, volatile int32_t* addr) {
    return OSAtomicAdd32Barrier(value, (int32_t*)addr)-value;
}

int32_t android_atomic_and(int32_t value, volatile int32_t* addr) {
    return OSAtomicAnd32OrigBarrier(value, (int32_t*)addr);
}

int32_t android_atomic_or(int32_t value, volatile int32_t* addr) {
    return OSAtomicOr32OrigBarrier(value, (int32_t*)addr);
}

int32_t android_atomic_acquire_swap(int32_t value, volatile int32_t* addr) {
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (android_atomic_acquire_cas(oldValue, value, addr));
    return oldValue;
}

int32_t android_atomic_release_swap(int32_t value, volatile int32_t* addr) {
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (android_atomic_release_cas(oldValue, value, addr));
    return oldValue;
}

int android_atomic_release_cas(int32_t oldvalue, int32_t newvalue, volatile int32_t* addr) {
    /* OS X CAS returns zero on failure; invert to return zero on success */
    return OSAtomicCompareAndSwap32Barrier(oldvalue, newvalue, (int32_t*)addr) == 0;
}

int android_atomic_acquire_cas(int32_t oldvalue, int32_t newvalue,
        volatile int32_t* addr) {
    int result = (OSAtomicCompareAndSwap32(oldvalue, newvalue, (int32_t*)addr) == 0);
    if (result == 0) {
        /* success, perform barrier */
        OSMemoryBarrier();
    }
    return result;
}

/*****************************************************************************/
#elif defined(__i386__) || defined(__x86_64__)

int32_t android_atomic_acquire_load(volatile int32_t* addr) {
    int32_t value = *addr;
    ANDROID_MEMBAR_FULL();
    return value;
}

int32_t android_atomic_release_load(volatile int32_t* addr) {
    ANDROID_MEMBAR_FULL();
    return *addr;
}

void android_atomic_acquire_store(int32_t value, volatile int32_t* addr) {
    *addr = value;
    ANDROID_MEMBAR_FULL();
}

void android_atomic_release_store(int32_t value, volatile int32_t* addr) {
    ANDROID_MEMBAR_FULL();
    *addr = value;
}

int32_t android_atomic_inc(volatile int32_t* addr) {
    return android_atomic_add(1, addr);
}

int32_t android_atomic_dec(volatile int32_t* addr) {
    return android_atomic_add(-1, addr);
}

int32_t android_atomic_add(int32_t value, volatile int32_t* addr) {
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (android_atomic_release_cas(oldValue, oldValue+value, addr));
    return oldValue;
}

int32_t android_atomic_and(int32_t value, volatile int32_t* addr) {
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (android_atomic_release_cas(oldValue, oldValue&value, addr));
    return oldValue;
}

int32_t android_atomic_or(int32_t value, volatile int32_t* addr) {
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (android_atomic_release_cas(oldValue, oldValue|value, addr));
    return oldValue;
}

/* returns 0 on successful swap */
static inline int cas(int32_t oldvalue, int32_t newvalue,
        volatile int32_t* addr) {
    int xchg;
    asm volatile
    (
    "   lock; cmpxchg %%ecx, (%%edx);"
    "   setne %%al;"
    "   andl $1, %%eax"
    : "=a" (xchg)
    : "a" (oldvalue), "c" (newvalue), "d" (addr)
    );
    return xchg;
}

int32_t android_atomic_acquire_swap(int32_t value, volatile int32_t* addr) {
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (cas(oldValue, value, addr));
    ANDROID_MEMBAR_FULL();
    return oldValue;
}

int32_t android_atomic_release_swap(int32_t value, volatile int32_t* addr) {
    ANDROID_MEMBAR_FULL();
    int32_t oldValue;
    do {
        oldValue = *addr;
    } while (cas(oldValue, value, addr));
    return oldValue;
}

int android_atomic_acquire_cas(int32_t oldvalue, int32_t newvalue,
        volatile int32_t* addr) {
    int xchg = cas(oldvalue, newvalue, addr);
    if (xchg == 0)
        ANDROID_MEMBAR_FULL();
    return xchg;
}

int android_atomic_release_cas(int32_t oldvalue, int32_t newvalue,
        volatile int32_t* addr) {
    ANDROID_MEMBAR_FULL();
    int xchg = cas(oldvalue, newvalue, addr);
    return xchg;
}


/*****************************************************************************/
#elif __arm__
// implementation for ARM is in atomic-android-arm.s.

/*****************************************************************************/
#elif __sh__
// implementation for SuperH is in atomic-android-sh.c.

#else

#error "Unsupported atomic operations for this platform"

#endif

