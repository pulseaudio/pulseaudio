#ifndef foologhfoo
#define foologhfoo

#include "gcc-printf.h"

enum pa_log_target {
    PA_LOG_SYSLOG,
    PA_LOG_STDERR,
    PA_LOG_USER,
};

void pa_log_set_ident(const char *p);
void pa_log_set_target(enum pa_log_target t, void (*func)(const char*s));
                       
void pa_log(const char *format, ...)  PA_GCC_PRINTF_ATTR(1,2);

#endif
