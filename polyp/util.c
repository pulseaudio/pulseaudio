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
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/resource.h>
#include <limits.h>
#include <unistd.h>
#include <grp.h>
#include <netdb.h>

#include <samplerate.h>

#include "util.h"
#include "xmalloc.h"
#include "log.h"

#define PA_RUNTIME_PATH_PREFIX "/tmp/polypaudio-"

/** Make a file descriptor nonblock. Doesn't do any error checking */
void pa_make_nonblock_fd(int fd) {
    int v;
    assert(fd >= 0);

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
}

/** Creates a directory securely */
int pa_make_secure_dir(const char* dir) {
    struct stat st;
    assert(dir);

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

/* Creates a the parent directory of the specified path securely */
int pa_make_secure_parent_dir(const char *fn) {
    int ret = -1;
    char *slash, *dir = pa_xstrdup(fn);
    
    if (!(slash = strrchr(dir, '/')))
        goto finish;
    *slash = 0;
    
    if (pa_make_secure_dir(dir) < 0)
        goto finish;

    ret = 0;
    
finish:
    pa_xfree(dir);
    return ret;
}


/** Calls read() in a loop. Makes sure that as much as 'size' bytes,
 * unless EOF is reached or an error occured */
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

/** Similar to pa_loop_read(), but wraps write() */
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

/* Print a warning messages in case that the given signal is not
 * blocked or trapped */
void pa_check_signal_is_blocked(int sig) {
    struct sigaction sa;
    sigset_t set;

    /* If POSIX threads are supported use thread-aware
     * pthread_sigmask() function, to check if the signal is
     * blocked. Otherwise fall back to sigprocmask() */
    
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

    /* Check whether the signal is trapped */
    
    if (sigaction(sig, NULL, &sa) < 0) {
        pa_log(__FILE__": sigaction() failed: %s\n", strerror(errno));
        return;
    }
        
    if (sa.sa_handler != SIG_DFL)
        return;
    
    pa_log(__FILE__": WARNING: %s is not trapped. This might cause malfunction!\n", pa_strsignal(sig));
}

/* The following function is based on an example from the GNU libc
 * documentation. This function is similar to GNU's asprintf(). */
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

/* Same as the previous function, but use a va_list instead of an
 * ellipsis */
char *pa_vsprintf_malloc(const char *format, va_list ap) {
    int  size = 100;
    char *c = NULL;
    
    assert(format);
    
    for(;;) {
        int r;
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

/* Return the current username in the specified string buffer. */
char *pa_get_user_name(char *s, size_t l) {
    struct passwd pw, *r;
    char buf[1024];
    char *p;
    assert(s && l > 0);

    if (!(p = getenv("USER")) && !(p = getenv("LOGNAME")) && !(p = getenv("USERNAME"))) {
        
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
        }

    return pa_strlcpy(s, p, l);
    }

/* Return the current hostname in the specified buffer. */
char *pa_get_host_name(char *s, size_t l) {
    assert(s && l > 0);
    if (gethostname(s, l) < 0) {
        pa_log(__FILE__": gethostname(): %s\n", strerror(errno));
        return NULL;
    }
    s[l-1] = 0;
    return s;
}

/* Return the home directory of the current user */
char *pa_get_home_dir(char *s, size_t l) {
    char *e;
    char buf[1024];
    struct passwd pw, *r;
    assert(s && l);

    if ((e = getenv("HOME")))
        return pa_strlcpy(s, e, l);

    if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &r) != 0 || !r) {
        pa_log(__FILE__": getpwuid_r() failed\n");
        return NULL;
    }

    return pa_strlcpy(s, r->pw_dir, l);
}

/* Similar to OpenBSD's strlcpy() function */
char *pa_strlcpy(char *b, const char *s, size_t l) {
    assert(b && s && l > 0);

    strncpy(b, s, l);
    b[l-1] = 0;
    return b;
}

int pa_gettimeofday(struct timeval *tv) {
#ifdef HAVE_GETTIMEOFDAY
    return gettimeofday(tv, NULL);
#else
#error "Platform lacks gettimeofday() or equivalent function."
#endif
}

/* Calculate the difference between the two specfified timeval
 * timestamsps. */
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

/* Compare the two timeval structs and return 0 when equal, negative when a < b, positive otherwse */
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

/* Return the time difference between now and the specified timestamp */
pa_usec_t pa_timeval_age(const struct timeval *tv) {
    struct timeval now;
    assert(tv);
    pa_gettimeofday(&now);
    return pa_timeval_diff(&now, tv);
}

/* Add the specified time inmicroseconds to the specified timeval structure */
void pa_timeval_add(struct timeval *tv, pa_usec_t v) {
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
}

#define NICE_LEVEL (-15)

/* Raise the priority of the current process as much as possible and
sensible: set the nice level to -15 and enable realtime scheduling if
supported.*/
void pa_raise_priority(void) {

    if (setpriority(PRIO_PROCESS, 0, NICE_LEVEL) < 0)
        pa_log_warn(__FILE__": setpriority() failed: %s\n", strerror(errno));
    else 
        pa_log_info(__FILE__": Successfully gained nice level %i.\n", NICE_LEVEL); 
    
#ifdef _POSIX_PRIORITY_SCHEDULING
    {
        struct sched_param sp;

        if (sched_getparam(0, &sp) < 0) {
            pa_log(__FILE__": sched_getparam() failed: %s\n", strerror(errno));
            return;
        }
        
        sp.sched_priority = 1;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
            pa_log_warn(__FILE__": sched_setscheduler() failed: %s\n", strerror(errno));
            return;
        }

        pa_log_info(__FILE__": Successfully enabled SCHED_FIFO scheduling.\n"); 
    }
#endif
}

/* Reset the priority to normal, inverting the changes made by pa_raise_priority() */
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

/* Set the FD_CLOEXEC flag for a fd */
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

/* Return the binary file name of the current process. Works on Linux
 * only. This shoul be used for eyecandy only, don't rely on return
 * non-NULL! */
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

/* Return a pointer to the filename inside a path (which is the last
 * component). */
char *pa_path_get_filename(const char *p) {
    char *fn;

    if ((fn = strrchr(p, '/')))
        return fn+1;

    return (char*) p;
}

/* Try to parse a boolean string value.*/
int pa_parse_boolean(const char *v) {
    
    if (!strcmp(v, "1") || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' || !strcasecmp(v, "on"))
        return 1;
    else if (!strcmp(v, "0") || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' || !strcasecmp(v, "off"))
        return 0;

    return -1;
}

/* Split the specified string wherever one of the strings in delimiter
 * occurs. Each time it is called returns a newly allocated string
 * with pa_xmalloc(). The variable state points to, should be
 * initiallized to NULL before the first call. */
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

/* What is interpreted as whitespace? */
#define WHITESPACE " \t\n"

/* Split a string into words. Otherwise similar to pa_split(). */
char *pa_split_spaces(const char *c, const char **state) {
    const char *current = *state ? *state : c;
    size_t l;

    if (!*current || *c == 0)
        return NULL;

    current += strspn(current, WHITESPACE);
    l = strcspn(current, WHITESPACE);

    *state = current+l;

    return pa_xstrndup(current, l);
}

/* Return the name of an UNIX signal. Similar to GNU's strsignal() */
const char *pa_strsignal(int sig) {
    switch(sig) {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
#ifdef SIGUSR1
        case SIGUSR1: return "SIGUSR1";
#endif
#ifdef SIGUSR2
        case SIGUSR2: return "SIGUSR2";
#endif
#ifdef SIGXCPU
        case SIGXCPU: return "SIGXCPU";
#endif
#ifdef SIGPIPE
        case SIGPIPE: return "SIGPIPE";
#endif
#ifdef SIGCHLD
        case SIGCHLD: return "SIGCHLD";
#endif
#ifdef SIGHUP
        case SIGHUP: return "SIGHUP";
#endif
        default: return "UNKNOWN SIGNAL";
    }
}


/* Check whether the specified GID and the group name match */
static int is_group(gid_t gid, const char *name) {
    struct group group, *result = NULL;
    long n;
    void *data;
    int r = -1;

#ifdef HAVE_GETGRGID_R
#ifdef _SC_GETGR_R_SIZE_MAX
    n = sysconf(_SC_GETGR_R_SIZE_MAX);
#else
    n = -1;
#endif
    if (n < 0) n = 512;
    data = pa_xmalloc(n);

    if (getgrgid_r(gid, &group, data, n, &result) < 0 || !result) {
        pa_log(__FILE__ ": getgrgid_r(%u) failed: %s\n", gid, strerror(errno));
        goto finish;
    }

    
    r = strcmp(name, result->gr_name) == 0;
    
finish:
    pa_xfree(data);
#else
    /* XXX Not thread-safe, but needed on OSes (e.g. FreeBSD 4.X) that do not
     * support getgrgid_r. */
    if ((result = getgrgid(gid)) == NULL) {
	pa_log(__FILE__ ": getgrgid(%u) failed: %s\n", gid, strerror(errno));
	goto finish;
    }

    r = strcmp(name, result->gr_name) == 0;

finish:
#endif
    
    return r;
}

/* Check the current user is member of the specified group */
int pa_uid_in_group(const char *name, gid_t *gid) {
    gid_t *gids, tgid;
    long n = sysconf(_SC_NGROUPS_MAX);
    int r = -1, i;

    assert(n > 0);
    
    gids = pa_xmalloc(sizeof(gid_t)*n);
    
    if ((n = getgroups(n, gids)) < 0) {
        pa_log(__FILE__": getgroups() failed: %s\n", strerror(errno));
        goto finish;
    }

    for (i = 0; i < n; i++) {
        if (is_group(gids[i], name) > 0) {
            *gid = gids[i];
            r = 1;
            goto finish;
        }
    }

    if (is_group(tgid = getgid(), name) > 0) {
        *gid = tgid;
        r = 1;
        goto finish;
    }

    r = 0;
    
finish:

    pa_xfree(gids);
    return r;
}

/* Lock or unlock a file entirely. (advisory) */
int pa_lock_fd(int fd, int b) {
    struct flock flock;

    /* Try a R/W lock first */
    
    flock.l_type = b ? F_WRLCK : F_UNLCK;
    flock.l_whence = SEEK_SET;
    flock.l_start = 0;
    flock.l_len = 0;

    if (fcntl(fd, F_SETLKW, &flock) >= 0)
        return 0;

    /* Perhaps the file descriptor qas opened for read only, than try again with a read lock. */
    if (b && errno == EBADF) {
        flock.l_type = F_RDLCK;
        if (fcntl(fd, F_SETLKW, &flock) >= 0)
            return 0;
    }
        
    pa_log(__FILE__": %slock failed: %s\n", !b ? "un" : "", strerror(errno));
    return -1;
}

/* Remove trailing newlines from a string */
char* pa_strip_nl(char *s) {
    assert(s);

    s[strcspn(s, "\r\n")] = 0;
    return s;
}

/* Create a temporary lock file and lock it. */
int pa_lock_lockfile(const char *fn) {
    int fd = -1;
    assert(fn);

    for (;;) {
        struct stat st;
        
        if ((fd = open(fn, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR)) < 0) {
            pa_log(__FILE__": failed to create lock file '%s': %s\n", fn, strerror(errno));
            goto fail;
        }
        
        if (pa_lock_fd(fd, 1) < 0) {
            pa_log(__FILE__": failed to lock file '%s'.\n", fn);
            goto fail;
        }
        
        if (fstat(fd, &st) < 0) {
            pa_log(__FILE__": failed to fstat() file '%s'.\n", fn);
            goto fail;
        }

        /* Check wheter the file has been removed meanwhile. When yes, restart this loop, otherwise, we're done */
        if (st.st_nlink >= 1)
            break;
            
        if (pa_lock_fd(fd, 0) < 0) {
            pa_log(__FILE__": failed to unlock file '%s'.\n", fn);
            goto fail;
        }
        
        if (close(fd) < 0) {
            pa_log(__FILE__": failed to close file '%s'.\n", fn);
            goto fail;
        }

        fd = -1;
    }
        
    return fd;

fail:

    if (fd >= 0)
        close(fd);

    return -1;
}

/* Unlock a temporary lcok file */
int pa_unlock_lockfile(const char *fn, int fd) {
    int r = 0;
    assert(fn && fd >= 0);

    if (unlink(fn) < 0) {
        pa_log_warn(__FILE__": WARNING: unable to remove lock file '%s': %s\n", fn, strerror(errno));
        r = -1;
    }
    
    if (pa_lock_fd(fd, 0) < 0) {
        pa_log_warn(__FILE__": WARNING: failed to unlock file '%s'.\n", fn);
        r = -1;
    }

    if (close(fd) < 0) {
        pa_log_warn(__FILE__": WARNING: failed to close lock file '%s': %s\n", fn, strerror(errno));
        r = -1;
    }

    return r;
}

/* Try to open a configuration file. If "env" is specified, open the
 * value of the specified environment variable. Otherwise look for a
 * file "local" in the home directory or a file "global" in global
 * file system. If "result" is non-NULL, a pointer to a newly
 * allocated buffer containing the used configuration file is
 * stored there.*/
FILE *pa_open_config_file(const char *global, const char *local, const char *env, char **result) {
    const char *e;
    char h[PATH_MAX];

    if (env && (e = getenv(env))) {
        if (result)
            *result = pa_xstrdup(e);
        return fopen(e, "r");
    }

    if (local && pa_get_home_dir(h, sizeof(h))) {
        FILE *f;
        char *l;
        
        l = pa_sprintf_malloc("%s/%s", h, local);
        f = fopen(l, "r");

        if (f || errno != ENOENT) {
            if (result)
                *result = l;
            else
                pa_xfree(l);
            return f;
        }
        
        pa_xfree(l);
    }

    if (!global) {
        if (result)
            *result = NULL;
        errno = ENOENT;
        return NULL;
    }

    if (result)
        *result = pa_xstrdup(global);
    
    return fopen(global, "r");
}
                 
/* Format the specified data as a hexademical string */
char *pa_hexstr(const uint8_t* d, size_t dlength, char *s, size_t slength) {
    size_t i = 0, j = 0;
    const char hex[] = "0123456789abcdef";
    assert(d && s && slength > 0);

    while (i < dlength && j+3 <= slength) {
        s[j++] = hex[*d >> 4];
        s[j++] = hex[*d & 0xF];

        d++;
        i++;
    }

    s[j < slength ? j : slength] = 0;
    return s;
}

/* Convert a hexadecimal digit to a number or -1 if invalid */
static int hexc(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    return -1;
}

/* Parse a hexadecimal string as created by pa_hexstr() to a BLOB */
size_t pa_parsehex(const char *p, uint8_t *d, size_t dlength) {
    size_t j = 0;
    assert(p && d);

    while (j < dlength && *p) {
        int b;

        if ((b = hexc(*(p++))) < 0)
            return (size_t) -1;
        
        d[j] = (uint8_t) (b << 4);

        if (!*p)
            return (size_t) -1;

        if ((b = hexc(*(p++))) < 0)
            return (size_t) -1;

        d[j] |= (uint8_t) b;
        j++;
    }

    return j;
}

/* Return the fully qualified domain name in *s */
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

/* Returns nonzero when *s starts with *pfx */
int pa_startswith(const char *s, const char *pfx) {
    size_t l;
    assert(s && pfx);
    l = strlen(pfx);

    return strlen(s) >= l && strncmp(s, pfx, l) == 0;
}

/* if fn is null return the polypaudio run time path in s (/tmp/polypaudio)
 * if fn is non-null and starts with / return fn in s
 * otherwise append fn to the run time path and return it in s */
char *pa_runtime_path(const char *fn, char *s, size_t l) {
    char u[256];

    if (fn && *fn == '/')
        return pa_strlcpy(s, fn, l);
    
    snprintf(s, l, PA_RUNTIME_PATH_PREFIX"%s%s%s", pa_get_user_name(u, sizeof(u)), fn ? "/" : "", fn ? fn : "");
    return s;
}

/* Wait t milliseconds */
int pa_msleep(unsigned long t) {
    struct timespec ts;

    ts.tv_sec = t/1000;
    ts.tv_nsec = (t % 1000) * 1000000;

    return nanosleep(&ts, NULL);
}

/* Convert the string s to a signed integer in *ret_i */
int pa_atoi(const char *s, int32_t *ret_i) {
    char *x = NULL;
    long l;
    assert(s && ret_i);

    l = strtol(s, &x, 0);

    if (!x || *x)
        return -1;

    *ret_i = (int32_t) l;
    
    return 0;
}

/* Convert the string s to an unsigned integer in *ret_u */
int pa_atou(const char *s, uint32_t *ret_u) {
    char *x = NULL;
    unsigned long l;
    assert(s && ret_u);

    l = strtoul(s, &x, 0);

    if (!x || *x)
        return -1;

    *ret_u = (uint32_t) l;
    
    return 0;
}
