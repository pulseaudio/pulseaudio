/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/resource.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


#include <samplerate.h>

#include "util.h"
#include "xmalloc.h"
#include "log.h"

void pa_make_nonblock_fd(int fd) {
    int v;

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
}

int pa_make_secure_dir(const char* dir) {
    struct stat st;

    if (mkdir(dir, 0700) < 0) 
        if (errno != EEXIST)
            return -1;
    
    if (lstat(dir, &st) < 0) 
        goto fail;
    
    if (!S_ISDIR(st.st_mode) || (st.st_uid != getuid()) || ((st.st_mode & 0777) != 0700))
        goto fail;
    
    return 0;
    
fail:
    rmdir(dir);
    return -1;
}

ssize_t pa_loop_read(int fd, void*data, size_t size) {
    ssize_t ret = 0;
    assert(fd >= 0 && data && size);

    while (size > 0) {
        ssize_t r;

        if ((r = read(fd, data, size)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data = (uint8_t*) data + r;
        size -= r;
    }

    return ret;
}

ssize_t pa_loop_write(int fd, const void*data, size_t size) {
    ssize_t ret = 0;
    assert(fd >= 0 && data && size);

    while (size > 0) {
        ssize_t r;

        if ((r = write(fd, data, size)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data = (uint8_t*) data + r;
        size -= r;
    }

    return ret;
}

void pa_check_signal_is_blocked(int sig) {
    struct sigaction sa;
    sigset_t set;

#ifdef HAVE_PTHREAD    
    if (pthread_sigmask(SIG_SETMASK, NULL, &set) < 0) {
#endif
        if (sigprocmask(SIG_SETMASK, NULL, &set) < 0) {
            pa_log(__FILE__": sigprocmask() failed: %s\n", strerror(errno));
            return;
        }
#ifdef HAVE_PTHREAD
    }
#endif

    if (sigismember(&set, sig))
        return;
    
    if (sigaction(sig, NULL, &sa) < 0) {
        pa_log(__FILE__": sigaction() failed: %s\n", strerror(errno));
        return;
    }
        
    if (sa.sa_handler != SIG_DFL)
        return;
    
    pa_log(__FILE__": WARNING: %s is not trapped. This might cause malfunction!\n", pa_strsignal(sig));
}

/* The following is based on an example from the GNU libc documentation */
char *pa_sprintf_malloc(const char *format, ...) {
    int  size = 100;
    char *c = NULL;
    
    assert(format);
    
    for(;;) {
        int r;
        va_list ap;

        c = pa_xrealloc(c, size);

        va_start(ap, format);
        r = vsnprintf(c, size, format, ap);
        va_end(ap);
        
        if (r > -1 && r < size)
            return c;

        if (r > -1)    /* glibc 2.1 */
            size = r+1; 
        else           /* glibc 2.0 */
            size *= 2;
    }
}

char *pa_vsprintf_malloc(const char *format, va_list ap) {
    int  size = 100;
    char *c = NULL;
    
    assert(format);
    
    for(;;) {
        int r;
        va_list ap;

        c = pa_xrealloc(c, size);
        r = vsnprintf(c, size, format, ap);
        
        if (r > -1 && r < size)
            return c;

        if (r > -1)    /* glibc 2.1 */
            size = r+1; 
        else           /* glibc 2.0 */
            size *= 2;
    }
}


char *pa_get_user_name(char *s, size_t l) {
    struct passwd pw, *r;
    char buf[1024];
    char *p;

    if (!(p = getenv("USER")))
        if (!(p = getenv("LOGNAME")))
            if (!(p = getenv("USERNAME"))) {
                
                if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &r) != 0 || !r) {
                    snprintf(s, l, "%lu", (unsigned long) getuid());
                    return s;
                }
                
                p = r->pw_name;
            }
    
    snprintf(s, l, "%s", p);
    return s;
}

char *pa_get_host_name(char *s, size_t l) {
    gethostname(s, l);
    s[l-1] = 0;
    return s;
}

pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b) {
    pa_usec_t r;
    assert(a && b);

    if (pa_timeval_cmp(a, b) < 0) {
        const struct timeval *c;
        c = a;
        a = b;
        b = c;
    }

    r = ((pa_usec_t) a->tv_sec - b->tv_sec)* 1000000;

    if (a->tv_usec > b->tv_usec)
        r += ((pa_usec_t) a->tv_usec - b->tv_usec);
    else if (a->tv_usec < b->tv_usec)
        r -= ((pa_usec_t) b->tv_usec - a->tv_usec);

    return r;
}

int pa_timeval_cmp(const struct timeval *a, const struct timeval *b) {
    assert(a && b);

    if (a->tv_sec < b->tv_sec)
        return -1;

    if (a->tv_sec > b->tv_sec)
        return 1;

    if (a->tv_usec < b->tv_usec)
        return -1;

    if (a->tv_usec > b->tv_usec)
        return 1;

    return 0;
}

pa_usec_t pa_age(const struct timeval *tv) {
    struct timeval now;
    assert(tv);
    gettimeofday(&now, NULL);
    return pa_timeval_diff(&now, tv);
}

void pa_timeval_add(struct timeval *tv, pa_usec_t v) {
    unsigned long secs;
    assert(tv);
    
    secs = (v/1000000);
    tv->tv_sec += (unsigned long) secs;
    v -= secs*1000000;

    tv->tv_usec += v;

    while (tv->tv_usec >= 1000000) {
        tv->tv_sec++;
        tv->tv_usec -= 1000000;
    }
}

#define NICE_LEVEL (-15)

void pa_raise_priority(void) {
    if (setpriority(PRIO_PROCESS, 0, NICE_LEVEL) < 0)
        pa_log(__FILE__": setpriority() failed: %s\n", strerror(errno));
    else
        pa_log(__FILE__": Successfully gained nice level %i.\n", NICE_LEVEL);

#ifdef _POSIX_PRIORITY_SCHEDULING
    {
        struct sched_param sp;

        if (sched_getparam(0, &sp) < 0) {
            pa_log(__FILE__": sched_getparam() failed: %s\n", strerror(errno));
            return;
        }
        
        sp.sched_priority = 1;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
            pa_log(__FILE__": sched_setscheduler() failed: %s\n", strerror(errno));
            return;
        }

        pa_log(__FILE__": Successfully enabled SCHED_FIFO scheduling.\n");
    }
#endif
}

void pa_reset_priority(void) {
#ifdef _POSIX_PRIORITY_SCHEDULING
    {
        struct sched_param sp;
        sched_getparam(0, &sp);
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
    }
#endif

    setpriority(PRIO_PROCESS, 0, 0);
}

int pa_fd_set_cloexec(int fd, int b) {
    int v;
    assert(fd >= 0);

    if ((v = fcntl(fd, F_GETFD, 0)) < 0)
        return -1;
    
    v = (v & ~FD_CLOEXEC) | (b ? FD_CLOEXEC : 0);
    
    if (fcntl(fd, F_SETFD, v) < 0)
        return -1;
    
    return 0;
}

char *pa_get_binary_name(char *s, size_t l) {
    char path[PATH_MAX];
    int i;
    assert(s && l);

    /* This works on Linux only */
    
    snprintf(path, sizeof(path), "/proc/%u/exe", (unsigned) getpid());
    if ((i = readlink(path, s, l-1)) < 0)
        return NULL;

    s[i] = 0;
    return s;
}

char *pa_path_get_filename(const char *p) {
    char *fn;

    if ((fn = strrchr(p, '/')))
        return fn+1;

    return (char*) p;
}

int pa_parse_boolean(const char *v) {
    
    if (!strcmp(v, "1") || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' || !strcasecmp(v, "on"))
        return 1;
    else if (!strcmp(v, "0") || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' || !strcasecmp(v, "off"))
        return 0;

    return -1;
}

char *pa_split(const char *c, const char *delimiter, const char**state) {
    const char *current = *state ? *state : c;
    size_t l;

    if (!*current)
        return NULL;
    
    l = strcspn(current, delimiter);
    *state = current+l;

    if (**state)
        (*state)++;

    return pa_xstrndup(current, l);
}

#define WHITESPACE " \t\n"

char *pa_split_spaces(const char *c, const char **state) {
    const char *current = *state ? *state : c;
    size_t l;

    if (!*current)
        return NULL;

    current += strspn(current, WHITESPACE);
    l = strcspn(current, WHITESPACE);

    *state = current+l;

    return pa_xstrndup(current, l);
}

const char *pa_strsignal(int sig) {
    switch(sig) {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGUSR1: return "SIGUSR1";
        case SIGUSR2: return "SIGUSR2";
        case SIGXCPU: return "SIGXCPU";
        case SIGPIPE: return "SIGPIPE";
        case SIGCHLD: return "SIGCHLD";
        default: return "UNKNOWN SIGNAL";
    }
}

int pa_parse_resample_method(const char *string) {
    assert(string);

    if (!strcmp(string, "sinc-best-quality"))
        return SRC_SINC_BEST_QUALITY;
    else if (!strcmp(string, "sinc-medium-quality"))
        return SRC_SINC_MEDIUM_QUALITY;
    else if (!strcmp(string, "sinc-fastest"))
        return SRC_SINC_FASTEST;
    else if (!strcmp(string, "zero-order-hold"))
        return SRC_ZERO_ORDER_HOLD;
    else if (!strcmp(string, "linear"))
        return SRC_LINEAR;
    else
        return -1;
}
