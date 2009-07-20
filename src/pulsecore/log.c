/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/util.h>
#include <pulse/timeval.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/once.h>
#include <pulsecore/ratelimit.h>

#include "log.h"

#define ENV_LOG_SYSLOG "PULSE_LOG_SYSLOG"
#define ENV_LOG_LEVEL "PULSE_LOG"
#define ENV_LOG_COLORS "PULSE_LOG_COLORS"
#define ENV_LOG_PRINT_TIME "PULSE_LOG_TIME"
#define ENV_LOG_PRINT_FILE "PULSE_LOG_FILE"
#define ENV_LOG_PRINT_META "PULSE_LOG_META"
#define ENV_LOG_PRINT_LEVEL "PULSE_LOG_LEVEL"
#define ENV_LOG_BACKTRACE "PULSE_LOG_BACKTRACE"
#define ENV_LOG_BACKTRACE_SKIP "PULSE_LOG_BACKTRACE_SKIP"

static char *ident = NULL; /* in local charset format */
static pa_log_target_t target = PA_LOG_STDERR, target_override;
static pa_bool_t target_override_set = FALSE;
static pa_log_level_t maximum_level = PA_LOG_ERROR, maximum_level_override = PA_LOG_ERROR;
static unsigned show_backtrace = 0, show_backtrace_override = 0, skip_backtrace = 0;
static pa_log_flags_t flags = 0, flags_override = 0;

#ifdef HAVE_SYSLOG_H
static const int level_to_syslog[] = {
    [PA_LOG_ERROR] = LOG_ERR,
    [PA_LOG_WARN] = LOG_WARNING,
    [PA_LOG_NOTICE] = LOG_NOTICE,
    [PA_LOG_INFO] = LOG_INFO,
    [PA_LOG_DEBUG] = LOG_DEBUG
};
#endif

static const char level_to_char[] = {
    [PA_LOG_ERROR] = 'E',
    [PA_LOG_WARN] = 'W',
    [PA_LOG_NOTICE] = 'N',
    [PA_LOG_INFO] = 'I',
    [PA_LOG_DEBUG] = 'D'
};

void pa_log_set_ident(const char *p) {
    pa_xfree(ident);

    if (!(ident = pa_utf8_to_locale(p)))
        ident = pa_ascii_filter(p);
}

/* To make valgrind shut up. */
static void ident_destructor(void) PA_GCC_DESTRUCTOR;
static void ident_destructor(void) {
    if (!pa_in_valgrind())
        return;

    pa_xfree(ident);
}

void pa_log_set_level(pa_log_level_t l) {
    pa_assert(l < PA_LOG_LEVEL_MAX);

    maximum_level = l;
}

void pa_log_set_target(pa_log_target_t t) {
    pa_assert(t < PA_LOG_TARGET_MAX);

    target = t;
}

void pa_log_set_flags(pa_log_flags_t _flags, pa_log_merge_t merge) {
    pa_assert(!(_flags & ~(PA_LOG_COLORS|PA_LOG_PRINT_TIME|PA_LOG_PRINT_FILE|PA_LOG_PRINT_META|PA_LOG_PRINT_LEVEL)));

    if (merge == PA_LOG_SET)
        flags |= _flags;
    else if (merge == PA_LOG_UNSET)
        flags &= ~_flags;
    else
        flags = _flags;
}

void pa_log_set_show_backtrace(unsigned nlevels) {
    show_backtrace = nlevels;
}

void pa_log_set_skip_backtrace(unsigned nlevels) {
    skip_backtrace = nlevels;
}

#ifdef HAVE_EXECINFO_H

static char* get_backtrace(unsigned show_nframes) {
    void* trace[32];
    int n_frames;
    char **symbols, *e, *r;
    unsigned j, n, s;
    size_t a;

    pa_assert(show_nframes > 0);

    n_frames = backtrace(trace, PA_ELEMENTSOF(trace));

    if (n_frames <= 0)
        return NULL;

    symbols = backtrace_symbols(trace, n_frames);

    if (!symbols)
        return NULL;

    s = skip_backtrace;
    n = PA_MIN((unsigned) n_frames, s + show_nframes);

    a = 4;

    for (j = s; j < n; j++) {
        if (j > s)
            a += 2;
        a += strlen(pa_path_get_filename(symbols[j]));
    }

    r = pa_xnew(char, a);

    strcpy(r, " (");
    e = r + 2;

    for (j = s; j < n; j++) {
        const char *sym;

        if (j > s) {
            strcpy(e, "<<");
            e += 2;
        }

        sym = pa_path_get_filename(symbols[j]);

        strcpy(e, sym);
        e += strlen(sym);
    }

    strcpy(e, ")");

    free(symbols);

    return r;
}

#endif

static void init_defaults(void) {
    const char *e;

    if (!ident) {
        char binary[256];
        if (pa_get_binary_name(binary, sizeof(binary)))
            pa_log_set_ident(binary);
    }

    if (getenv(ENV_LOG_SYSLOG)) {
        target_override = PA_LOG_SYSLOG;
        target_override_set = TRUE;
    }

    if ((e = getenv(ENV_LOG_LEVEL))) {
        maximum_level_override = (pa_log_level_t) atoi(e);

        if (maximum_level_override >= PA_LOG_LEVEL_MAX)
            maximum_level_override = PA_LOG_LEVEL_MAX-1;
    }

    if (getenv(ENV_LOG_COLORS))
        flags_override |= PA_LOG_COLORS;

    if (getenv(ENV_LOG_PRINT_TIME))
        flags_override |= PA_LOG_PRINT_TIME;

    if (getenv(ENV_LOG_PRINT_FILE))
        flags_override |= PA_LOG_PRINT_FILE;

    if (getenv(ENV_LOG_PRINT_META))
        flags_override |= PA_LOG_PRINT_META;

    if (getenv(ENV_LOG_PRINT_LEVEL))
        flags_override |= PA_LOG_PRINT_LEVEL;

    if ((e = getenv(ENV_LOG_BACKTRACE))) {
        show_backtrace_override = (unsigned) atoi(e);

        if (show_backtrace_override <= 0)
            show_backtrace_override = 0;
    }

    if ((e = getenv(ENV_LOG_BACKTRACE_SKIP))) {
        skip_backtrace = (unsigned) atoi(e);

        if (skip_backtrace <= 0)
            skip_backtrace = 0;
    }
}

void pa_log_levelv_meta(
        pa_log_level_t level,
        const char*file,
        int line,
        const char *func,
        const char *format,
        va_list ap) {

    char *t, *n;
    int saved_errno = errno;
    char *bt = NULL;
    pa_log_target_t _target;
    pa_log_level_t _maximum_level;
    unsigned _show_backtrace;
    pa_log_flags_t _flags;

    /* We don't use dynamic memory allocation here to minimize the hit
     * in RT threads */
    char text[16*1024], location[128], timestamp[32];

    pa_assert(level < PA_LOG_LEVEL_MAX);
    pa_assert(format);

    PA_ONCE_BEGIN {
        init_defaults();
    } PA_ONCE_END;

    _target = target_override_set ? target_override : target;
    _maximum_level = PA_MAX(maximum_level, maximum_level_override);
    _show_backtrace = PA_MAX(show_backtrace, show_backtrace_override);
    _flags = flags | flags_override;

    if (PA_LIKELY(level > _maximum_level)) {
        errno = saved_errno;
        return;
    }

    pa_vsnprintf(text, sizeof(text), format, ap);

    if ((_flags & PA_LOG_PRINT_META) && file && line > 0 && func)
        pa_snprintf(location, sizeof(location), "[%s:%i %s()] ", file, line, func);
    else if ((_flags & (PA_LOG_PRINT_META|PA_LOG_PRINT_FILE)) && file)
        pa_snprintf(location, sizeof(location), "%s: ", pa_path_get_filename(file));
    else
        location[0] = 0;

    if (_flags & PA_LOG_PRINT_TIME) {
        static pa_usec_t start, last;
        pa_usec_t u, a, r;

        u = pa_rtclock_now();

        PA_ONCE_BEGIN {
            start = u;
            last = u;
        } PA_ONCE_END;

        r = u - last;
        a = u - start;

        /* This is not thread safe, but this is a debugging tool only
         * anyway. */
        last = u;

        pa_snprintf(timestamp, sizeof(timestamp), "(%4llu.%03llu|%4llu.%03llu) ",
                    (unsigned long long) (a / PA_USEC_PER_SEC),
                    (unsigned long long) (((a / PA_USEC_PER_MSEC)) % 1000),
                    (unsigned long long) (r / PA_USEC_PER_SEC),
                    (unsigned long long) (((r / PA_USEC_PER_MSEC)) % 1000));

    } else
        timestamp[0] = 0;

#ifdef HAVE_EXECINFO_H
    if (_show_backtrace > 0)
        bt = get_backtrace(_show_backtrace);
#endif

    if (!pa_utf8_valid(text))
        pa_logl(level, "Invalid UTF-8 string following below:");

    for (t = text; t; t = n) {
        if ((n = strchr(t, '\n'))) {
            *n = 0;
            n++;
        }

        /* We ignore strings only made out of whitespace */
        if (t[strspn(t, "\t ")] == 0)
            continue;

        switch (_target) {

            case PA_LOG_STDERR: {
                const char *prefix = "", *suffix = "", *grey = "";
                char *local_t;

#ifndef OS_IS_WIN32
                /* Yes indeed. Useless, but fun! */
                if ((_flags & PA_LOG_COLORS) && isatty(STDERR_FILENO)) {
                    if (level <= PA_LOG_ERROR)
                        prefix = "\x1B[1;31m";
                    else if (level <= PA_LOG_WARN)
                        prefix = "\x1B[1m";

                    if (bt)
                        grey = "\x1B[2m";

                    if (grey[0] || prefix[0])
                        suffix = "\x1B[0m";
                }
#endif

                /* We shouldn't be using dynamic allocation here to
                 * minimize the hit in RT threads */
                if ((local_t = pa_utf8_to_locale(t)))
                    t = local_t;

                if (_flags & PA_LOG_PRINT_LEVEL)
                    fprintf(stderr, "%s%c: %s%s%s%s%s%s\n", timestamp, level_to_char[level], location, prefix, t, grey, pa_strempty(bt), suffix);
                else
                    fprintf(stderr, "%s%s%s%s%s%s%s\n", timestamp, location, prefix, t, grey, pa_strempty(bt), suffix);

                pa_xfree(local_t);

                break;
            }

#ifdef HAVE_SYSLOG_H
            case PA_LOG_SYSLOG: {
                char *local_t;

                openlog(ident, LOG_PID, LOG_USER);

                if ((local_t = pa_utf8_to_locale(t)))
                    t = local_t;

                syslog(level_to_syslog[level], "%s%s%s%s", timestamp, location, t, pa_strempty(bt));
                pa_xfree(local_t);

                break;
            }
#endif

            case PA_LOG_NULL:
            default:
                break;
        }
    }

    pa_xfree(bt);
    errno = saved_errno;
}

void pa_log_level_meta(
        pa_log_level_t level,
        const char*file,
        int line,
        const char *func,
        const char *format, ...) {

    va_list ap;
    va_start(ap, format);
    pa_log_levelv_meta(level, file, line, func, format, ap);
    va_end(ap);
}

void pa_log_levelv(pa_log_level_t level, const char *format, va_list ap) {
    pa_log_levelv_meta(level, NULL, 0, NULL, format, ap);
}

void pa_log_level(pa_log_level_t level, const char *format, ...) {
    va_list ap;

    va_start(ap, format);
    pa_log_levelv_meta(level, NULL, 0, NULL, format, ap);
    va_end(ap);
}

pa_bool_t pa_log_ratelimit(void) {
    /* Not more than 10 messages every 5s */
    static PA_DEFINE_RATELIMIT(ratelimit, 5 * PA_USEC_PER_SEC, 10);

    return pa_ratelimit_test(&ratelimit);
}
