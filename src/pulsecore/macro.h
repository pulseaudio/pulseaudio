#ifndef foopulsemacrohfoo
#define foopulsemacrohfoo

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
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <pulsecore/log.h>
#include <pulse/gccmacro.h>

#ifndef PACKAGE
#error "Please include config.h before including this file!"
#endif

#ifndef PA_LIKELY
#ifdef __GNUC__
#define PA_LIKELY(x) (__builtin_expect(!!(x),1))
#define PA_UNLIKELY(x) (__builtin_expect((x),0))
#else
#define PA_LIKELY(x) (x)
#define PA_UNLIKELY(x) (x)
#endif
#endif

#if defined(PAGE_SIZE)
#define PA_PAGE_SIZE ((size_t) PAGE_SIZE)
#elif defined(PAGESIZE)
#define PA_PAGE_SIZE ((size_t) PAGESIZE)
#elif defined(HAVE_SYSCONF)
#define PA_PAGE_SIZE ((size_t) (sysconf(_SC_PAGE_SIZE)))
#else
/* Let's hope it's like x86. */
#define PA_PAGE_SIZE ((size_t) 4096)
#endif

static inline size_t pa_align(size_t l) {
    return (((l + sizeof(void*) - 1) / sizeof(void*)) * sizeof(void*));
}
#define PA_ALIGN(x) (pa_align(x))

static inline void* pa_page_align_ptr(const void *p) {
    return (void*) (((size_t) p) & ~(PA_PAGE_SIZE-1));
}
#define PA_PAGE_ALIGN_PTR(x) (pa_page_align_ptr(x))

static inline size_t pa_page_align(size_t l) {
    return l & ~(PA_PAGE_SIZE-1);
}
#define PA_PAGE_ALIGN(x) (pa_page_align(x))

#define PA_ELEMENTSOF(x) (sizeof(x)/sizeof((x)[0]))

/* The users of PA_MIN and PA_MAX should be aware that these macros on
 * non-GCC executed code with side effects twice. It is thus
 * considered misuse to use code with side effects as arguments to MIN
 * and MAX. */

#ifdef __GNUC__
#define PA_MAX(a,b)                             \
    __extension__ ({ typeof(a) _a = (a);        \
            typeof(b) _b = (b);                 \
            _a > _b ? _a : _b;                  \
        })
#else
#define PA_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifdef __GNUC__
#define PA_MIN(a,b)                             \
    __extension__ ({ typeof(a) _a = (a);        \
            typeof(b) _b = (b);                 \
            _a < _b ? _a : _b;                  \
        })
#else
#define PA_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifdef __GNUC__
#define PA_CLAMP(x, low, high)                                          \
    __extension__ ({ typeof(x) _x = (x);                                \
            typeof(low) _low = (low);                                   \
            typeof(high) _high = (high);                                \
            ((_x > _high) ? _high : ((_x < _low) ? _low : _x));         \
        })
#else
#define PA_CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#endif

#ifdef __GNUC__
#define PA_CLAMP_UNLIKELY(x, low, high)                                 \
    __extension__ ({ typeof(x) _x = (x);                                \
            typeof(low) _low = (low);                                   \
            typeof(high) _high = (high);                                \
            (PA_UNLIKELY(_x > _high) ? _high : (PA_UNLIKELY(_x < _low) ? _low : _x)); \
        })
#else
#define PA_CLAMP_UNLIKELY(x, low, high) (PA_UNLIKELY((x) > (high)) ? (high) : (PA_UNLIKELY((x) < (low)) ? (low) : (x)))
#endif

/* We don't define a PA_CLAMP_LIKELY here, because it doesn't really
 * make sense: we cannot know if it is more likely that the values is
 * lower or greater than the boundaries.*/

/* This type is not intended to be used in exported APIs! Use classic "int" there! */
#ifdef HAVE_STD_BOOL
typedef _Bool pa_bool_t;
#else
typedef int pa_bool_t;
#endif

#ifndef FALSE
#define FALSE ((pa_bool_t) 0)
#endif

#ifndef TRUE
#define TRUE (!FALSE)
#endif

#ifdef __GNUC__
#define PA_PRETTY_FUNCTION __PRETTY_FUNCTION__
#else
#define PA_PRETTY_FUNCTION ""
#endif

#define pa_return_if_fail(expr)                                         \
    do {                                                                \
        if (PA_UNLIKELY(!(expr))) {                                     \
            pa_log_debug("Assertion '%s' failed at %s:%u, function %s.\n", #expr , __FILE__, __LINE__, PA_PRETTY_FUNCTION); \
            return;                                                     \
        }                                                               \
    } while(FALSE)

#define pa_return_val_if_fail(expr, val)                                \
    do {                                                                \
        if (PA_UNLIKELY(!(expr))) {                                     \
            pa_log_debug("Assertion '%s' failed at %s:%u, function %s.\n", #expr , __FILE__, __LINE__, PA_PRETTY_FUNCTION); \
            return (val);                                               \
        }                                                               \
    } while(FALSE)

#define pa_return_null_if_fail(expr) pa_return_val_if_fail(expr, NULL)

/* An assert which guarantees side effects of x, i.e. is never
 * optimized away */
#define pa_assert_se(expr)                                              \
    do {                                                                \
        if (PA_UNLIKELY(!(expr))) {                                     \
            pa_log_error("Assertion '%s' failed at %s:%u, function %s(). Aborting.", #expr , __FILE__, __LINE__, PA_PRETTY_FUNCTION); \
            abort();                                                    \
        }                                                               \
    } while (FALSE)

/* An assert that may be optimized away by defining NDEBUG */
#ifdef NDEBUG
#define pa_assert(expr) do {} while (FALSE)
#else
#define pa_assert(expr) pa_assert_se(expr)
#endif

#define pa_assert_not_reached()                                         \
    do {                                                                \
        pa_log_error("Code should not be reached at %s:%u, function %s(). Aborting.", __FILE__, __LINE__, PA_PRETTY_FUNCTION); \
        abort();                                                        \
    } while (FALSE)

#define PA_PTR_TO_UINT(p) ((unsigned int) (unsigned long) (p))
#define PA_UINT_TO_PTR(u) ((void*) (unsigned long) (u))

#define PA_PTR_TO_UINT32(p) ((uint32_t) PA_PTR_TO_UINT(p))
#define PA_UINT32_TO_PTR(u) PA_UINT_TO_PTR((uint32_t) u)

#define PA_PTR_TO_INT(p) ((int) PA_PTR_TO_UINT(p))
#define PA_INT_TO_PTR(u) PA_UINT_TO_PTR((int) u)

#define PA_PTR_TO_INT32(p) ((int32_t) PA_PTR_TO_UINT(p))
#define PA_INT32_TO_PTR(u) PA_UINT_TO_PTR((int32_t) u)

#ifdef OS_IS_WIN32
#define PA_PATH_SEP "\\"
#define PA_PATH_SEP_CHAR '\\'
#else
#define PA_PATH_SEP "/"
#define PA_PATH_SEP_CHAR '/'
#endif

#if defined(__GNUC__) && defined(__ELF__)

#define PA_WARN_REFERENCE(sym, msg)                  \
    __asm__(".section .gnu.warning." #sym);          \
    __asm__(".asciz \"" msg "\"");                   \
    __asm__(".previous")

#else

#define PA_WARN_REFERENCE(sym, msg)

#endif

#endif
