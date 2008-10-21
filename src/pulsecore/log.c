/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/util.h>
#include <pulse/timeval.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/once.h>

#include "log.h"

#define ENV_LOGLEVEL "PULSE_LOG"
#define ENV_LOGMETA "PULSE_LOG_META"
#define ENV_LOGTIME "PULSE_LOG_TIME"
#define ENV_LOGBACKTRACE "PULSE_LOG_BACKTRACE"

static char *log_ident = NULL, *log_ident_local = NULL;
static pa_log_target_t log_target = PA_LOG_STDERR;
static pa_log_func_t user_log_func = NULL;
static pa_log_level_t maximal_level = PA_LOG_ERROR;
static unsigned show_backtrace = 0;
static pa_bool_t show_meta = FALSE;
static pa_bool_t show_time = FALSE;

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
    pa_xfree(log_ident);
    pa_xfree(log_ident_local);

    log_ident = pa_xstrdup(p);
    if (!(log_ident_local = pa_utf8_to_locale(log_ident)))
        log_ident_local = pa_xstrdup(log_ident);
}

/* To make valgrind shut up. */
static void ident_destructor(void) PA_GCC_DESTRUCTOR;
static void ident_destructor(void) {
    if (!pa_in_valgrind())
        return;

    pa_xfree(log_ident);
    pa_xfree(log_ident_local);
}

void pa_log_set_maximal_level(pa_log_level_t l) {
    pa_assert(l < PA_LOG_LEVEL_MAX);

    maximal_level = l;
}

void pa_log_set_target(pa_log_target_t t, pa_log_func_t func) {
    pa_assert(t == PA_LOG_USER || !func);

    log_target = t;
    user_log_func = func;
}

void pa_log_set_show_meta(pa_bool_t b) {
    show_meta = b;
}

void pa_log_set_show_time(pa_bool_t b) {
    show_time = b;
}

void pa_log_set_show_backtrace(unsigned nlevels) {
    show_backtrace = nlevels;
}

#ifdef HAVE_EXECINFO_H

static char* get_backtrace(unsigned show_nframes) {
    void* trace[32];
    int n_frames;
    char **symbols, *e, *r;
    unsigned j, n;
    size_t a;

    if (show_nframes <= 0)
        return NULL;

    n_frames = backtrace(trace, PA_ELEMENTSOF(trace));

    if (n_frames <= 0)
        return NULL;

    symbols = backtrace_symbols(trace, n_frames);

    if (!symbols)
        return NULL;

    n = PA_MIN((unsigned) n_frames, show_nframes);

    a = 4;

    for (j = 0; j < n; j++) {
        if (j > 0)
            a += 2;
        a += strlen(symbols[j]);
    }

    r = pa_xnew(char, a);

    strcpy(r, " (");
    e = r + 2;

    for (j = 0; j < n; j++) {
        if (j > 0) {
            strcpy(e, "<<");
            e += 2;
        }

        strcpy(e, symbols[j]);
        e += strlen(symbols[j]);
    }

    strcpy(e, ")");

    free(symbols);

    return r;
}

#endif

void pa_log_levelv_meta(
        pa_log_level_t level,
        const char*file,
        int line,
        const char *func,
        const char *format,
        va_list ap) {

    const char *e;
    char *t, *n;
    int saved_errno = errno;
    char *bt = NULL;
    pa_log_level_t ml;
#ifdef HAVE_EXECINFO_H
    unsigned show_bt;
#endif

    /* We don't use dynamic memory allocation here to minimize the hit
     * in RT threads */
    char text[4096], location[128], timestamp[32];

    pa_assert(level < PA_LOG_LEVEL_MAX);
    pa_assert(format);

    ml = maximal_level;

    if (PA_UNLIKELY((e = getenv(ENV_LOGLEVEL)))) {
        pa_log_level_t eml = (pa_log_level_t) atoi(e);

        if (eml > ml)
            ml = eml;
    }

    if (PA_LIKELY(level > ml)) {
        errno = saved_errno;
        return;
    }

    pa_vsnprintf(text, sizeof(text), format, ap);

    if ((show_meta || getenv(ENV_LOGMETA)) && file && line > 0 && func)
        pa_snprintf(location, sizeof(location), "[%s:%i %s()] ", file, line, func);
    else if (file)
        pa_snprintf(location, sizeof(location), "%s: ", pa_path_get_filename(file));
    else
        location[0] = 0;

    if (show_time || getenv(ENV_LOGTIME)) {
        static pa_usec_t start, last;
        pa_usec_t u, a, r;

        u = pa_rtclock_usec();

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
    show_bt = show_backtrace;

    if ((e = getenv(ENV_LOGBACKTRACE))) {
        unsigned ebt = (unsigned) atoi(e);

        if (ebt > show_bt)
            show_bt = ebt;
    }

    bt = get_backtrace(show_bt);
#endif

    if (!pa_utf8_valid(text))
        pa_log_level(level, __FILE__": invalid UTF-8 string following below:");

    for (t = text; t; t = n) {
        if ((n = strchr(t, '\n'))) {
            *n = 0;
            n++;
        }

        if (!*t)
            continue;

        switch (log_target) {
            case PA_LOG_STDERR: {
                const char *prefix = "", *suffix = "", *grey = "";
                char *local_t;

#ifndef OS_IS_WIN32
                /* Yes indeed. Useless, but fun! */
                if (isatty(STDERR_FILENO)) {
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
                local_t = pa_utf8_to_locale(t);
                if (!local_t)
                    fprintf(stderr, "%s%c: %s%s%s%s%s%s\n", timestamp, level_to_char[level], location, prefix, t, grey, pa_strempty(bt), suffix);
                else {
                    fprintf(stderr, "%s%c: %s%s%s%s%s%s\n", timestamp, level_to_char[level], location, prefix, local_t, grey, pa_strempty(bt), suffix);
                    pa_xfree(local_t);
                }

                break;
            }

#ifdef HAVE_SYSLOG_H
            case PA_LOG_SYSLOG: {
                char *local_t;

                openlog(log_ident_local ? log_ident_local : "???", LOG_PID, LOG_USER);

                local_t = pa_utf8_to_locale(t);
                if (!local_t)
                    syslog(level_to_syslog[level], "%s%s%s%s", timestamp, location, t, pa_strempty(bt));
                else {
                    syslog(level_to_syslog[level], "%s%s%s%s", timestamp, location, local_t, pa_strempty(bt));
                    pa_xfree(local_t);
                }

                closelog();
                break;
            }
#endif

            case PA_LOG_USER: {
                char x[1024];

                pa_snprintf(x, sizeof(x), "%s%s%s", timestamp, location, t);
                user_log_func(level, x);

                break;
            }

            case PA_LOG_NULL:
            default:
                break;
        }
    }

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
