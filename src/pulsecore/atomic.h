#ifndef foopulseatomichfoo
#define foopulseatomichfoo

/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <atomic_ops.h>

/* atomic_ops guarantees us that sizeof(AO_t) == sizeof(void*).
 *
 * It is not guaranteed however, that sizeof(AO_t) == sizeof(size_t).
 * however very likely. */

typedef struct pa_atomic_int {
    volatile AO_t value;
} pa_atomic_int_t;

/* For now we do only full memory barriers. Eventually we might want
 * to support more elaborate memory barriers, in which case we will add
 * suffixes to the function names */

static inline int pa_atomic_load(const pa_atomic_int_t *a) {
    return (int) AO_load_full((AO_t*) &a->value);
}

static inline void pa_atomic_store(pa_atomic_int_t *a, int i) {
    AO_store_full(&a->value, (AO_t) i);
}

static inline int pa_atomic_add(pa_atomic_int_t *a, int i) {
    return AO_fetch_and_add_full(&a->value, (AO_t) i);
}

static inline int pa_atomic_inc(pa_atomic_int_t *a) {
    return AO_fetch_and_add1_full(&a->value);
}

static inline int pa_atomic_dec(pa_atomic_int_t *a) {
    return AO_fetch_and_sub1_full(&a->value);
}

static inline int pa_atomic_cmpxchg(pa_atomic_int_t *a, int old_i, int new_i) {
    return AO_compare_and_swap_full(&a->value, old_i, new_i);
}

typedef struct pa_atomic_ptr {
    volatile AO_t value;
} pa_atomic_ptr_t;

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
