#ifndef foopulseatomichfoo
#define foopulseatomichfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

/*
 * atomic_ops guarantees us that sizeof(AO_t) == sizeof(void*).  It is
 * not guaranteed however, that sizeof(AO_t) == sizeof(size_t).
 * however very likely.
 *
 * For now we do only full memory barriers. Eventually we might want
 * to support more elaborate memory barriers, in which case we will add
 * suffixes to the function names.
 *
 * On gcc >= 4.1 we use the builtin atomic functions. otherwise we use
 * libatomic_ops
 */

#ifndef PACKAGE
#error "Please include config.h before including this file!"
#endif

#ifdef HAVE_ATOMIC_BUILTINS

/* __sync based implementation */

typedef struct pa_atomic {
    volatile int value;
} pa_atomic_t;

#define PA_ATOMIC_INIT(v) { .value = (v) }

static inline int pa_atomic_load(const pa_atomic_t *a) {
    __sync_synchronize();
    return a->value;
}

static inline void pa_atomic_store(pa_atomic_t *a, int i) {
    a->value = i;
    __sync_synchronize();
}

/* Returns the previously set value */
static inline int pa_atomic_add(pa_atomic_t *a, int i) {
    return __sync_fetch_and_add(&a->value, i);
}

/* Returns the previously set value */
static inline int pa_atomic_sub(pa_atomic_t *a, int i) {
    return __sync_fetch_and_sub(&a->value, i);
}

/* Returns the previously set value */
static inline int pa_atomic_inc(pa_atomic_t *a) {
    return pa_atomic_add(a, 1);
}

/* Returns the previously set value */
static inline int pa_atomic_dec(pa_atomic_t *a) {
    return pa_atomic_sub(a, 1);
}

/* Returns non-zero when the operation was successful. */
static inline int pa_atomic_cmpxchg(pa_atomic_t *a, int old_i, int new_i) {
    return __sync_bool_compare_and_swap(&a->value, old_i, new_i);
}

typedef struct pa_atomic_ptr {
    volatile unsigned long value;
} pa_atomic_ptr_t;

#define PA_ATOMIC_PTR_INIT(v) { .value = (long) (v) }

static inline void* pa_atomic_ptr_load(const pa_atomic_ptr_t *a) {
    __sync_synchronize();
    return (void*) a->value;
}

static inline void pa_atomic_ptr_store(pa_atomic_ptr_t *a, void *p) {
    a->value = (unsigned long) p;
    __sync_synchronize();
}

static inline int pa_atomic_ptr_cmpxchg(pa_atomic_ptr_t *a, void *old_p, void* new_p) {
    return __sync_bool_compare_and_swap(&a->value, (long) old_p, (long) new_p);
}

#elif defined(__GNUC__) && (defined(__amd64__) || defined(__x86_64__))

#error "The native atomic operations implementation for AMD64 has not been tested. libatomic_ops is known to not work properly on AMD64 and your gcc version is too old for the gcc-builtin atomic ops support. You have three options now: make the native atomic operations implementation for AMD64 work, fix libatomic_ops, or upgrade your GCC."

/* Addapted from glibc */

typedef struct pa_atomic {
    volatile int value;
} pa_atomic_t;

#define PA_ATOMIC_INIT(v) { .value = (v) }

static inline int pa_atomic_load(const pa_atomic_t *a) {
    return a->value;
}

static inline void pa_atomic_store(pa_atomic_t *a, int i) {
    a->value = i;
}

static inline int pa_atomic_add(pa_atomic_t *a, int i) {
    int result;

    __asm __volatile ("lock; xaddl %0, %1"
                      : "=r" (result), "=m" (a->value)
                      : "0" (i), "m" (a->value));

    return result;
}

static inline int pa_atomic_sub(pa_atomic_t *a, int i) {
    return pa_atomic_add(a, -i);
}

static inline int pa_atomic_inc(pa_atomic_t *a) {
    return pa_atomic_add(a, 1);
}

static inline int pa_atomic_dec(pa_atomic_t *a) {
    return pa_atomic_sub(a, 1);
}

static inline int pa_atomic_cmpxchg(pa_atomic_t *a, int old_i, int new_i) {
    int result;

    __asm__ __volatile__ ("lock; cmpxchgl %2, %1"
                          : "=a" (result), "=m" (a->value)
                          : "r" (new_i), "m" (a->value), "0" (old_i));

    return result == oldval;
}

typedef struct pa_atomic_ptr {
    volatile unsigned long value;
} pa_atomic_ptr_t;

#define PA_ATOMIC_PTR_INIT(v) { .value = (long) (v) }

static inline void* pa_atomic_ptr_load(const pa_atomic_ptr_t *a) {
    return (void*) a->value;
}

static inline void pa_atomic_ptr_store(pa_atomic_ptr_t *a, void *p) {
    a->value = (unsigned long) p;
}

static inline int pa_atomic_ptr_cmpxchg(pa_atomic_ptr_t *a, void *old_p, void* new_p) {
    void *result;

    __asm__ __volatile__ ("lock; cmpxchgq %q2, %1"
                          : "=a" (result), "=m" (a->value)
                          : "r" (new_p), "m" (a->value), "0" (old_p));

    return result;
}

#else

/* libatomic_ops based implementation */

#include <atomic_ops.h>

typedef struct pa_atomic {
    volatile AO_t value;
} pa_atomic_t;

#define PA_ATOMIC_INIT(v) { .value = (v) }

static inline int pa_atomic_load(const pa_atomic_t *a) {
    return (int) AO_load_full((AO_t*) &a->value);
}

static inline void pa_atomic_store(pa_atomic_t *a, int i) {
    AO_store_full(&a->value, (AO_t) i);
}

static inline int pa_atomic_add(pa_atomic_t *a, int i) {
    return AO_fetch_and_add_full(&a->value, (AO_t) i);
}

static inline int pa_atomic_sub(pa_atomic_t *a, int i) {
    return AO_fetch_and_add_full(&a->value, (AO_t) -i);
}

static inline int pa_atomic_inc(pa_atomic_t *a) {
    return AO_fetch_and_add1_full(&a->value);
}

static inline int pa_atomic_dec(pa_atomic_t *a) {
    return AO_fetch_and_sub1_full(&a->value);
}

static inline int pa_atomic_cmpxchg(pa_atomic_t *a, int old_i, int new_i) {
    return AO_compare_and_swap_full(&a->value, old_i, new_i);
}

typedef struct pa_atomic_ptr {
    volatile AO_t value;
} pa_atomic_ptr_t;

#define PA_ATOMIC_PTR_INIT(v) { .value = (AO_t) (v) }

static inline void* pa_atomic_ptr_load(const pa_atomic_ptr_t *a) {
    return (void*) AO_load_full((AO_t*) &a->value);
}

static inline void pa_atomic_ptr_store(pa_atomic_ptr_t *a, void *p) {
    AO_store_full(&a->value, (AO_t) p);
}

static inline int pa_atomic_ptr_cmpxchg(pa_atomic_ptr_t *a, void *old_p, void* new_p) {
    return AO_compare_and_swap_full(&a->value, (AO_t) old_p, (AO_t) new_p);
}

#endif

#endif
