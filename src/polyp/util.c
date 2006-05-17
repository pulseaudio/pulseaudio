/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
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
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#ifdef HAVE_GRP_H
#include <grp.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include <polyp/xmalloc.h>
#include <polypcore/log.h>
#include <polypcore/core-util.h>

#include "util.h"

#ifndef OS_IS_WIN32
#define PATH_SEP '/'
#else
#define PATH_SEP '\\'
#endif

char *pa_get_user_name(char *s, size_t l) {
    char *p;
    char buf[1024];

#ifdef HAVE_PWD_H
    struct passwd pw, *r;
#endif

    assert(s && l > 0);

    if (!(p = getenv("USER")) && !(p = getenv("LOGNAME")) && !(p = getenv("USERNAME"))) {
#ifdef HAVE_PWD_H
        
#ifdef HAVE_GETPWUID_R
        if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &r) != 0 || !r) {
#else
        /* XXX Not thread-safe, but needed on OSes (e.g. FreeBSD 4.X)
            * that do not support getpwuid_r. */
        if ((r = getpwuid(getuid())) == NULL) {
#endif
            snprintf(s, l, "%lu", (unsigned long) getuid());
            return s;
        }
        
        p = r->pw_name;

#elif defined(OS_IS_WIN32) /* HAVE_PWD_H */
        DWORD size = sizeof(buf);

        if (!GetUserName(buf, &size))
            return NULL;

        p = buf;

#else /* HAVE_PWD_H */
        return NULL;
#endif /* HAVE_PWD_H */
    }

    return pa_strlcpy(s, p, l);
}

char *pa_get_host_name(char *s, size_t l) {
    assert(s && l > 0);
    if (gethostname(s, l) < 0) {
        pa_log(__FILE__": gethostname(): %s", strerror(errno));
        return NULL;
    }
    s[l-1] = 0;
    return s;
}

char *pa_get_home_dir(char *s, size_t l) {
    char *e;

#ifdef HAVE_PWD_H
    char buf[1024];
    struct passwd pw, *r;
#endif

    assert(s && l);

    if ((e = getenv("HOME")))
        return pa_strlcpy(s, e, l);

    if ((e = getenv("USERPROFILE")))
        return pa_strlcpy(s, e, l);

#ifdef HAVE_PWD_H
#ifdef HAVE_GETPWUID_R
    if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &r) != 0 || !r) {
        pa_log(__FILE__": getpwuid_r() failed");
#else
    /* XXX Not thread-safe, but needed on OSes (e.g. FreeBSD 4.X)
        * that do not support getpwuid_r. */
    if ((r = getpwuid(getuid())) == NULL) {
        pa_log(__FILE__": getpwuid_r() failed");
#endif
        return NULL;
    }

    return pa_strlcpy(s, r->pw_dir, l);
#else /* HAVE_PWD_H */
    return NULL;
#endif
}

struct timeval *pa_gettimeofday(struct timeval *tv) {
#ifdef HAVE_GETTIMEOFDAY
    assert(tv);
    
    return gettimeofday(tv, NULL) < 0 ? NULL : tv;
#elif defined(OS_IS_WIN32)
    /*
     * Copied from implementation by Steven Edwards (LGPL).
     * Found on wine mailing list.
     */

#if defined(_MSC_VER) || defined(__BORLANDC__)
#define EPOCHFILETIME (116444736000000000i64)
#else
#define EPOCHFILETIME (116444736000000000LL)
#endif

    FILETIME        ft;
    LARGE_INTEGER   li;
    __int64         t;

    assert(tv);

    GetSystemTimeAsFileTime(&ft);
    li.LowPart  = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    t  = li.QuadPart;       /* In 100-nanosecond intervals */
    t -= EPOCHFILETIME;     /* Offset to the Epoch time */
    t /= 10;                /* In microseconds */
    tv->tv_sec  = (long)(t / 1000000);
    tv->tv_usec = (long)(t % 1000000);

    return tv;
#else
#error "Platform lacks gettimeofday() or equivalent function."
#endif
}

pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b) {
    pa_usec_t r;
    assert(a && b);

    /* Check which whan is the earlier time and swap the two arguments if reuqired. */
    if (pa_timeval_cmp(a, b) < 0) {
        const struct timeval *c;
        c = a;
        a = b;
        b = c;
    }

    /* Calculate the second difference*/
    r = ((pa_usec_t) a->tv_sec - b->tv_sec)* 1000000;

    /* Calculate the microsecond difference */
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

pa_usec_t pa_timeval_age(const struct timeval *tv) {
    struct timeval now;
    assert(tv);
    
    return pa_timeval_diff(pa_gettimeofday(&now), tv);
}

struct timeval* pa_timeval_add(struct timeval *tv, pa_usec_t v) {
    unsigned long secs;
    assert(tv);
    
    secs = (v/1000000);
    tv->tv_sec += (unsigned long) secs;
    v -= secs*1000000;

    tv->tv_usec += v;

    /* Normalize */
    while (tv->tv_usec >= 1000000) {
        tv->tv_sec++;
        tv->tv_usec -= 1000000;
    }

    return tv;
}

char *pa_get_binary_name(char *s, size_t l) {

#ifdef HAVE_READLINK
    char path[PATH_MAX];
    int i;
    assert(s && l);

    /* This works on Linux only */
    
    snprintf(path, sizeof(path), "/proc/%u/exe", (unsigned) getpid());
    if ((i = readlink(path, s, l-1)) < 0)
        return NULL;

    s[i] = 0;
    return s;
#elif defined(OS_IS_WIN32)
    char path[PATH_MAX];
    if (!GetModuleFileName(NULL, path, PATH_MAX))
        return NULL;
    pa_strlcpy(s, pa_path_get_filename(path), l);
    return s;
#else
    return NULL;
#endif
}

const char *pa_path_get_filename(const char *p) {
    char *fn;

    if ((fn = strrchr(p, PATH_SEP)))
        return fn+1;

    return (const char*) p;
}

char *pa_get_fqdn(char *s, size_t l) {
    char hn[256];
#ifdef HAVE_GETADDRINFO    
    struct addrinfo *a, hints;
#endif

    if (!pa_get_host_name(hn, sizeof(hn)))
        return NULL;

#ifdef HAVE_GETADDRINFO
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_CANONNAME;
    
    if (getaddrinfo(hn, NULL, &hints, &a) < 0 || !a || !a->ai_canonname || !*a->ai_canonname)
        return pa_strlcpy(s, hn, l);

    pa_strlcpy(s, a->ai_canonname, l);
    freeaddrinfo(a);
    return s;
#else
    return pa_strlcpy(s, hn, l);
#endif
}

int pa_msleep(unsigned long t) {
#ifdef OS_IS_WIN32
    Sleep(t);
    return 0;
#elif defined(HAVE_NANOSLEEP)
    struct timespec ts;

    ts.tv_sec = t/1000;
    ts.tv_nsec = (t % 1000) * 1000000;

    return nanosleep(&ts, NULL);
#else
#error "Platform lacks a sleep function."
#endif
}
