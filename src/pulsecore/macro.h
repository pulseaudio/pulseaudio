#ifndef foopulsemacrohfoo
#define foopulsemacrohfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>
#include <assert.h>
#include <pulsecore/log.h>

static inline size_t pa_align(size_t l) {
    return (((l + sizeof(void*) - 1) / sizeof(void*)) * sizeof(void*));
}

#define PA_ALIGN(x) (pa_align(x))

#define PA_ELEMENTSOF(x) (sizeof(x)/sizeof((x)[0]))

#define SA_MAX(a, b) ((a) > (b) ? (a) : (b))
#define SA_MIN(a, b) ((a) < (b) ? (a) : (b))

#ifdef __GNUC__
#define PA_PRETTY_FUNCTION __PRETTY_FUNCTION__
#else
#define PA_PRETTY_FUNCTION ""
#endif

#define pa_return_if_fail(expr) \
    do { \
        if (!(expr)) { \
            pa_log_debug("%s: Assertion <%s> failed.\n", PA_PRETTY_FUNCTION, #expr ); \
            return; \
        } \
    } while(0)

#define pa_return_val_if_fail(expr, val) \
    do { \
        if (!(expr)) { \
            pa_log_debug("%s: Assertion <%s> failed.\n", PA_PRETTY_FUNCTION, #expr ); \
            return (val); \
        } \
    } while(0)

#define pa_return_null_if_fail(expr) pa_return_val_if_fail(expr, NULL)

#define pa_assert assert

#define pa_assert_not_reached() pa_assert(!"Should not be reached.")

/* An assert which guarantees side effects of x */
#ifdef NDEBUG
#define pa_assert_se(x) x
#else
#define pa_assert_se(x) pa_assert(x)
#endif

#define PA_PTR_TO_UINT(p) ((unsigned int) (unsigned long) (p))
#define PA_UINT_TO_PTR(u) ((void*) (unsigned long) (u))

#define PA_PTR_TO_UINT32(p) ((uint32_t) PA_PTR_TO_UINT(p))
#define PA_UINT32_TO_PTR(u) PA_UINT_TO_PTR((uint32_t) u)

#define PA_PTR_TO_INT(p) ((int) PA_PTR_TO_UINT(p))
#define PA_INT_TO_PTR(u) PA_UINT_TO_PTR((int) u)

#define PA_PTR_TO_INT32(p) ((int32_t) PA_PTR_TO_UINT(p))
#define PA_INT32_TO_PTR(u) PA_UINT_TO_PTR((int32_t) u)

#endif
