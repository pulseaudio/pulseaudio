/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>

#include "log.h"
#include "xmalloc.h"
#include "util.h"

static char *log_ident = NULL;
static enum pa_log_target log_target = PA_LOG_STDERR;
static void (*user_log_func)(const char *s) = NULL;

void pa_log_set_ident(const char *p) {
    if (log_ident)
        pa_xfree(log_ident);

    log_ident = pa_xstrdup(p);
}

void pa_log_set_target(enum pa_log_target t, void (*func)(const char*s)) {
    assert(t == PA_LOG_USER || !func);
    log_target = t;
    user_log_func = func;
}

void pa_log(const char *format, ...) {
    va_list ap;
    va_start(ap, format);

    switch (log_target) {
        case PA_LOG_STDERR:
            vfprintf(stderr, format, ap);
            break;
        case PA_LOG_SYSLOG:
            openlog(log_ident ? log_ident : "???", LOG_PID, LOG_USER);
            vsyslog(LOG_INFO, format, ap);
            closelog();
            break;
        case PA_LOG_USER: {
            char *t = pa_vsprintf_malloc(format, ap);
            assert(user_log_func);
            user_log_func(t);
	    pa_xfree(t);
        }
        case PA_LOG_NULL:
            break;
    }

    va_end(ap);
}
