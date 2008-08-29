#ifndef foologhfoo
#define foologhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdarg.h>
#include <stdlib.h>
#include <pulse/gccmacro.h>

/* A simple logging subsystem */

/* Where to log to */
typedef enum pa_log_target {
    PA_LOG_STDERR,  /* default */
    PA_LOG_SYSLOG,
    PA_LOG_USER,    /* to user specified function */
    PA_LOG_NULL     /* to /dev/null */
} pa_log_target_t;

typedef enum pa_log_level {
    PA_LOG_ERROR  = 0,    /* Error messages */
    PA_LOG_WARN   = 1,    /* Warning messages */
    PA_LOG_NOTICE = 2,    /* Notice messages */
    PA_LOG_INFO   = 3,    /* Info messages */
    PA_LOG_DEBUG  = 4,    /* debug message */
    PA_LOG_LEVEL_MAX
} pa_log_level_t;

/* Set an identification for the current daemon. Used when logging to syslog. */
void pa_log_set_ident(const char *p);

typedef void (*pa_log_func_t)(pa_log_level_t t, const char*s);

/* Set another log target. If t is PA_LOG_USER you may specify a function that is called every log string */
void pa_log_set_target(pa_log_target_t t, pa_log_func_t func);

/* Minimal log level */
void pa_log_set_maximal_level(pa_log_level_t l);

void pa_log_level_meta(
        pa_log_level_t level,
        const char*file,
        int line,
        const char *func,
        const char *format, ...) PA_GCC_PRINTF_ATTR(5,6);
void pa_log_levelv_meta(
        pa_log_level_t level,
        const char*file,
        int line,
        const char *func,
        const char *format,
        va_list ap);

void pa_log_level(pa_log_level_t level, const char *format, ...) PA_GCC_PRINTF_ATTR(2,3);
void pa_log_levelv(pa_log_level_t level, const char *format, va_list ap);

#if __STDC_VERSION__ >= 199901L

/* ISO varargs available */

#define pa_log_debug(...)  pa_log_level_meta(PA_LOG_DEBUG,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pa_log_info(...)   pa_log_level_meta(PA_LOG_INFO,   __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pa_log_notice(...) pa_log_level_meta(PA_LOG_NOTICE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pa_log_warn(...)   pa_log_level_meta(PA_LOG_WARN,   __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pa_log_error(...)  pa_log_level_meta(PA_LOG_ERROR,  __FILE__, __LINE__, __func__, __VA_ARGS__)

#else

#define LOG_FUNC(suffix, level) \
PA_GCC_UNUSED static void pa_log_##suffix(const char *format, ...) { \
    va_list ap; \
    va_start(ap, format); \
    pa_log_levelv_meta(level, NULL, 0, NULL, format, ap); \
    va_end(ap); \
}

LOG_FUNC(debug, PA_LOG_DEBUG)
LOG_FUNC(info, PA_LOG_INFO)
LOG_FUNC(notice, PA_LOG_NOTICE)
LOG_FUNC(warn, PA_LOG_WARN)
LOG_FUNC(error, PA_LOG_ERROR)

#endif

#define pa_log pa_log_error

#endif
