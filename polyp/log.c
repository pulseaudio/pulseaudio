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
        }
    }

    va_end(ap);
}
