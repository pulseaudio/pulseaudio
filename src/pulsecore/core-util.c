/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
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

#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#ifdef HAVE_GRP_H
#include <grp.h>
#endif

#include <samplerate.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/winsock.h>
#include <pulsecore/log.h>

#include "core-util.h"

/* Not all platforms have this */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef OS_IS_WIN32
#define PA_USER_RUNTIME_PATH_PREFIX "/tmp/pulse-"
#define PATH_SEP '/'
#else
#define PA_USER_RUNTIME_PATH_PREFIX "%TEMP%\\pulse-"
#define PATH_SEP '\\'
#endif

#ifdef OS_IS_WIN32

#define PULSE_ROOTENV "PULSE_ROOT"

int pa_set_root(HANDLE handle) {
    char library_path[MAX_PATH + sizeof(PULSE_ROOTENV) + 1], *sep;

    strcpy(library_path, PULSE_ROOTENV "=");

    if (!GetModuleFileName(handle, library_path + sizeof(PULSE_ROOTENV), MAX_PATH))
        return 0;

    sep = strrchr(library_path, '\\');
    if (sep)
        *sep = '\0';

    if (_putenv(library_path) < 0)
        return 0;

    return 1;
}

#endif

/** Make a file descriptor nonblock. Doesn't do any error checking */
void pa_make_nonblock_fd(int fd) {
#ifdef O_NONBLOCK
    int v;
    assert(fd >= 0);

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
#elif defined(OS_IS_WIN32)
    u_long arg = 1;
    if (ioctlsocket(fd, FIONBIO, &arg) < 0) {
        if (WSAGetLastError() == WSAENOTSOCK)
            pa_log_warn("WARNING: Only sockets can be made non-blocking!");
    }
#else
    pa_log_warn("WARNING: Non-blocking I/O not supported.!");
#endif
}

/** Creates a directory securely */
int pa_make_secure_dir(const char* dir, mode_t m, uid_t uid, gid_t gid) {
    struct stat st;
    int r;
    
    assert(dir);

#ifdef OS_IS_WIN32
    r = mkdir(dir);
#else
    {
    mode_t u;
    u = umask(~m);
    r = mkdir(dir, m);
    umask(u);
    }
#endif
    
    if (r < 0 && errno != EEXIST)
        return -1;

#ifdef HAVE_CHOWN
    if (uid == (uid_t)-1)
        uid = getuid();
    if (gid == (gid_t)-1)
        gid = getgid();
    chown(dir, uid, gid);
#endif
    
#ifdef HAVE_CHMOD
    chmod(dir, m);
#endif
    
#ifdef HAVE_LSTAT
    if (lstat(dir, &st) < 0)
#else
    if (stat(dir, &st) < 0)
#endif
        goto fail;
    
#ifndef OS_IS_WIN32
    if (!S_ISDIR(st.st_mode) ||
        (st.st_uid != uid) ||
        (st.st_gid != gid) ||
        ((st.st_mode & 0777) != m)) {
        errno = EACCES;
        goto fail;
    }
#else
	pa_log_warn("secure directory creation not supported on Win32.");
#endif
    
    return 0;
    
fail:
    rmdir(dir);
    return -1;
}

/* Return a newly allocated sting containing the parent directory of the specified file */
char *pa_parent_dir(const char *fn) {
    char *slash, *dir = pa_xstrdup(fn);

    if ((slash = (char*) pa_path_get_filename(dir)) == dir) {
        pa_xfree(dir);
        return NULL;
    }

    *(slash-1) = 0;
    return dir;
}

/* Creates a the parent directory of the specified path securely */
int pa_make_secure_parent_dir(const char *fn, mode_t m, uid_t uid, gid_t gid) {
    int ret = -1;
    char *dir;

    if (!(dir = pa_parent_dir(fn)))
        goto finish;
    
    if (pa_make_secure_dir(dir, m, uid, gid) < 0)
        goto finish;

    ret = 0;
    
finish:
    pa_xfree(dir);
    return ret;
}

/** Platform independent read function. Necessary since not all
 * systems treat all file descriptors equal. If type is
 * non-NULL it is used to cache the type of the fd. This is
 * useful for making sure that only a single syscall is executed per
 * function call. The variable pointed to should be initialized to 0
 * by the caller. */
ssize_t pa_read(int fd, void *buf, size_t count, int *type) {

#ifdef OS_IS_WIN32

    if (!type || *type == 0) {
        ssize_t r;
        
        if ((r = recv(fd, buf, count, 0)) >= 0)
            return r;

        if (WSAGetLastError() != WSAENOTSOCK) {
            errno = WSAGetLastError();
            return r;
        }

        if (type)
            *type = 1;
    }

#endif
    
    return read(fd, buf, count);
}

/** Similar to pa_read(), but handles writes */
ssize_t pa_write(int fd, const void *buf, size_t count, int *type) {

    if (!type || *type == 0) {
        ssize_t r;

        if ((r = send(fd, buf, count, MSG_NOSIGNAL)) >= 0)
            return r;
    
#ifdef OS_IS_WIN32
        if (WSAGetLastError() != WSAENOTSOCK) {
            errno = WSAGetLastError();
            return r;
        }
#else
        if (errno != ENOTSOCK)
            return r;
#endif

        if (type)
            *type = 1;
    }

    return write(fd, buf, count);
}

/** Calls read() in a loop. Makes sure that as much as 'size' bytes,
 * unless EOF is reached or an error occured */
ssize_t pa_loop_read(int fd, void*data, size_t size, int *type) {
    ssize_t ret = 0;
    int _type;
    
    assert(fd >= 0);
    assert(data);
    assert(size);

    if (!type) {
        _type = 0;
        type = &_type;
    }

    while (size > 0) {
        ssize_t r;

        if ((r = pa_read(fd, data, size, type)) < 0)
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
ssize_t pa_loop_write(int fd, const void*data, size_t size, int *type) {
    ssize_t ret = 0;
    int _type;

    assert(fd >= 0);
    assert(data);
    assert(size);

    if (!type) {
        _type = 0;
        type = &_type;
    }

    while (size > 0) {
        ssize_t r;

        if ((r = pa_write(fd, data, size, type)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data = (const uint8_t*) data + r;
        size -= r;
    }

    return ret;
}

/* Print a warning messages in case that the given signal is not
 * blocked or trapped */
void pa_check_signal_is_blocked(int sig) {
#ifdef HAVE_SIGACTION
    struct sigaction sa;
    sigset_t set;

    /* If POSIX threads are supported use thread-aware
     * pthread_sigmask() function, to check if the signal is
     * blocked. Otherwise fall back to sigprocmask() */
    
#ifdef HAVE_PTHREAD    
    if (pthread_sigmask(SIG_SETMASK, NULL, &set) < 0) {
#endif
        if (sigprocmask(SIG_SETMASK, NULL, &set) < 0) {
            pa_log("sigprocmask(): %s", pa_cstrerror(errno));
            return;
        }
#ifdef HAVE_PTHREAD
    }
#endif

    if (sigismember(&set, sig))
        return;

    /* Check whether the signal is trapped */
    
    if (sigaction(sig, NULL, &sa) < 0) {
        pa_log("sigaction(): %s", pa_cstrerror(errno));
        return;
    }
        
    if (sa.sa_handler != SIG_DFL)
        return;
    
    pa_log("WARNING: %s is not trapped. This might cause malfunction!", pa_strsignal(sig));
#else /* HAVE_SIGACTION */
    pa_log("WARNING: %s might not be trapped. This might cause malfunction!", pa_strsignal(sig));
#endif
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
        va_list aq;

        va_copy(aq, ap);

        c = pa_xrealloc(c, size);
        r = vsnprintf(c, size, format, aq);

        va_end(aq);
        
        if (r > -1 && r < size)
            return c;

        if (r > -1)    /* glibc 2.1 */
            size = r+1; 
        else           /* glibc 2.0 */
            size *= 2;
    }
}

/* Similar to OpenBSD's strlcpy() function */
char *pa_strlcpy(char *b, const char *s, size_t l) {
    assert(b && s && l > 0);

    strncpy(b, s, l);
    b[l-1] = 0;
    return b;
}

#define NICE_LEVEL (-15)

/* Raise the priority of the current process as much as possible and
sensible: set the nice level to -15 and enable realtime scheduling if
supported.*/
void pa_raise_priority(void) {

#ifdef HAVE_SYS_RESOURCE_H
    if (setpriority(PRIO_PROCESS, 0, NICE_LEVEL) < 0)
        pa_log_warn("setpriority(): %s", pa_cstrerror(errno));
    else 
        pa_log_info("Successfully gained nice level %i.", NICE_LEVEL); 
#endif
    
#ifdef _POSIX_PRIORITY_SCHEDULING
    {
        struct sched_param sp;

        if (sched_getparam(0, &sp) < 0) {
            pa_log("sched_getparam(): %s", pa_cstrerror(errno));
            return;
        }
        
        sp.sched_priority = 1;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
            pa_log_warn("sched_setscheduler(): %s", pa_cstrerror(errno));
            return;
        }

        pa_log_info("Successfully enabled SCHED_FIFO scheduling."); 
    }
#endif

#ifdef OS_IS_WIN32
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        pa_log_warn("SetPriorityClass() failed: 0x%08X", GetLastError());
    else
        pa_log_info("Successfully gained high priority class."); 
#endif
}

/* Reset the priority to normal, inverting the changes made by pa_raise_priority() */
void pa_reset_priority(void) {
#ifdef OS_IS_WIN32
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
#endif

#ifdef _POSIX_PRIORITY_SCHEDULING
    {
        struct sched_param sp;
        sched_getparam(0, &sp);
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
    }
#endif

#ifdef HAVE_SYS_RESOURCE_H
    setpriority(PRIO_PROCESS, 0, 0);
#endif
}

/* Set the FD_CLOEXEC flag for a fd */
int pa_fd_set_cloexec(int fd, int b) {

#ifdef FD_CLOEXEC
    int v;
    assert(fd >= 0);

    if ((v = fcntl(fd, F_GETFD, 0)) < 0)
        return -1;
    
    v = (v & ~FD_CLOEXEC) | (b ? FD_CLOEXEC : 0);
    
    if (fcntl(fd, F_SETFD, v) < 0)
        return -1;
#endif    

    return 0;
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

#ifdef HAVE_GRP_H

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
        pa_log("getgrgid_r(%u): %s", (unsigned)gid, pa_cstrerror(errno));
        goto finish;
    }

    r = strcmp(name, result->gr_name) == 0;
    
finish:
    pa_xfree(data);
#else
    /* XXX Not thread-safe, but needed on OSes (e.g. FreeBSD 4.X) that do not
     * support getgrgid_r. */
    if ((result = getgrgid(gid)) == NULL) {
        pa_log("getgrgid(%u): %s", gid, pa_cstrerror(errno));
        goto finish;
    }

    r = strcmp(name, result->gr_name) == 0;

finish:
#endif
    
    return r;
}

/* Check the current user is member of the specified group */
int pa_own_uid_in_group(const char *name, gid_t *gid) {
    GETGROUPS_T *gids, tgid;
    int n = sysconf(_SC_NGROUPS_MAX);
    int r = -1, i;

    assert(n > 0);
    
    gids = pa_xmalloc(sizeof(GETGROUPS_T)*n);
    
    if ((n = getgroups(n, gids)) < 0) {
        pa_log("getgroups(): %s", pa_cstrerror(errno));
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

/* Check whether the specifc user id is a member of the specified group */
int pa_uid_in_group(uid_t uid, const char *name) {
    char *g_buf, *p_buf;
    long g_n, p_n;
    struct group grbuf, *gr;
    char **i;
    int r = -1;
    
    g_n = sysconf(_SC_GETGR_R_SIZE_MAX);
    g_buf = pa_xmalloc(g_n);

    p_n = sysconf(_SC_GETPW_R_SIZE_MAX);
    p_buf = pa_xmalloc(p_n);
    
    if (getgrnam_r(name, &grbuf, g_buf, (size_t) g_n, &gr) != 0 || !gr)
        goto finish;

    r = 0;
    for (i = gr->gr_mem; *i; i++) {
        struct passwd pwbuf, *pw;
        
        if (getpwnam_r(*i, &pwbuf, p_buf, (size_t) p_n, &pw) != 0 || !pw)
            continue;

        if (pw->pw_uid == uid) {
            r = 1;
            break;
        }
    }

finish:
    pa_xfree(g_buf);
    pa_xfree(p_buf);

    return r;
}

/* Get the GID of a gfiven group, return (gid_t) -1 on failure. */
gid_t pa_get_gid_of_group(const char *name) {
    gid_t ret = (gid_t) -1;
    char *g_buf;
    long g_n;
    struct group grbuf, *gr;

    g_n = sysconf(_SC_GETGR_R_SIZE_MAX);
    g_buf = pa_xmalloc(g_n);

    if (getgrnam_r(name, &grbuf, g_buf, (size_t) g_n, &gr) != 0 || !gr)
        goto finish;

    ret = gr->gr_gid;

finish:
    pa_xfree(g_buf);
    return ret;
}

int pa_check_in_group(gid_t g) {
    gid_t gids[NGROUPS_MAX];
    int r;

    if ((r = getgroups(NGROUPS_MAX, gids)) < 0)
        return -1;

    for (; r > 0; r--)
        if (gids[r-1] == g)
            return 1;

    return 0;
}

#else /* HAVE_GRP_H */

int pa_own_uid_in_group(const char *name, gid_t *gid) {
    return -1;
    
}

int pa_uid_in_group(uid_t uid, const char *name) {
    return -1;
}

gid_t pa_get_gid_of_group(const char *name) {
    return (gid_t) -1;
}

int pa_check_in_group(gid_t g) {
    return -1;
}

#endif

/* Lock or unlock a file entirely.
  (advisory on UNIX, mandatory on Windows) */
int pa_lock_fd(int fd, int b) {
#ifdef F_SETLKW
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
        
    pa_log("%slock: %s", !b? "un" : "",
        pa_cstrerror(errno));
#endif

#ifdef OS_IS_WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);

    if (b && LockFile(h, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF))
        return 0;
    if (!b && UnlockFile(h, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF))
        return 0;

    pa_log("%slock failed: 0x%08X", !b ? "un" : "", GetLastError());
#endif

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
            pa_log("failed to create lock file '%s': %s", fn,
                pa_cstrerror(errno));
            goto fail;
        }
        
        if (pa_lock_fd(fd, 1) < 0) {
            pa_log("failed to lock file '%s'.", fn);
            goto fail;
        }
        
        if (fstat(fd, &st) < 0) {
            pa_log("failed to fstat() file '%s'.", fn);
            goto fail;
        }

        /* Check wheter the file has been removed meanwhile. When yes, restart this loop, otherwise, we're done */
        if (st.st_nlink >= 1)
            break;
            
        if (pa_lock_fd(fd, 0) < 0) {
            pa_log("failed to unlock file '%s'.", fn);
            goto fail;
        }
        
        if (close(fd) < 0) {
            pa_log("failed to close file '%s'.", fn);
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
        pa_log_warn("WARNING: unable to remove lock file '%s': %s",
            fn, pa_cstrerror(errno));
        r = -1;
    }
    
    if (pa_lock_fd(fd, 0) < 0) {
        pa_log_warn("WARNING: failed to unlock file '%s'.", fn);
        r = -1;
    }

    if (close(fd) < 0) {
        pa_log_warn("WARNING: failed to close lock file '%s': %s",
            fn, pa_cstrerror(errno));
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
FILE *pa_open_config_file(const char *global, const char *local, const char *env, char **result, const char *mode) {
    const char *fn;
    char h[PATH_MAX];

#ifdef OS_IS_WIN32
    char buf[PATH_MAX];

    if (!getenv(PULSE_ROOTENV))
        pa_set_root(NULL);
#endif

    if (env && (fn = getenv(env))) {
#ifdef OS_IS_WIN32
        if (!ExpandEnvironmentStrings(fn, buf, PATH_MAX))
            return NULL;
        fn = buf;
#endif

        if (result)
            *result = pa_xstrdup(fn);

        return fopen(fn, mode);
    }

    if (local) {
        const char *e;
        char *lfn = NULL;

        if ((e = getenv("PULSE_CONFIG_PATH")))
            fn = lfn = pa_sprintf_malloc("%s/%s", e, local);
        else if (pa_get_home_dir(h, sizeof(h)))
            fn = lfn = pa_sprintf_malloc("%s/.pulse/%s", h, local);

        if (lfn) {
            FILE *f;
        
#ifdef OS_IS_WIN32
            if (!ExpandEnvironmentStrings(lfn, buf, PATH_MAX))
                return NULL;
            fn = buf;
#endif
            
            if ((f = fopen(fn, mode)) || errno != ENOENT) {
                if (result)
                    *result = pa_xstrdup(fn);
                pa_xfree(lfn);
                return f;
            }
        
            pa_xfree(lfn);
        }
    }

    if (!global) {
        if (result)
            *result = NULL;
        errno = ENOENT;
        return NULL;
    }

#ifdef OS_IS_WIN32
    if (!ExpandEnvironmentStrings(global, buf, PATH_MAX))
        return NULL;
    global = buf;
#endif

    if (result)
        *result = pa_xstrdup(global);
    
    return fopen(global, mode);
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

/* Returns nonzero when *s starts with *pfx */
int pa_startswith(const char *s, const char *pfx) {
    size_t l;
    
    assert(s);
    assert(pfx);
    
    l = strlen(pfx);

    return strlen(s) >= l && strncmp(s, pfx, l) == 0;
}

/* Returns nonzero when *s ends with *sfx */
int pa_endswith(const char *s, const char *sfx) {
    size_t l1, l2;
    
    assert(s);
    assert(sfx);
    
    l1 = strlen(s);
    l2 = strlen(sfx);

    return l1 >= l2 && strcmp(s+l1-l2, sfx) == 0;
}

/* if fn is null return the PulseAudio run time path in s (/tmp/pulse)
 * if fn is non-null and starts with / return fn in s
 * otherwise append fn to the run time path and return it in s */
char *pa_runtime_path(const char *fn, char *s, size_t l) {
    const char *e;

#ifndef OS_IS_WIN32
    if (fn && *fn == '/')
#else
    if (fn && strlen(fn) >= 3 && isalpha(fn[0]) && fn[1] == ':' && fn[2] == '\\')
#endif
        return pa_strlcpy(s, fn, l);

    if ((e = getenv("PULSE_RUNTIME_PATH"))) {

        if (fn)    
            snprintf(s, l, "%s%c%s", e, PATH_SEP, fn);
        else
            snprintf(s, l, "%s", e);
        
    } else {
        char u[256];
        
        if (fn)    
            snprintf(s, l, "%s%s%c%s", PA_USER_RUNTIME_PATH_PREFIX, pa_get_user_name(u, sizeof(u)), PATH_SEP, fn);
        else
            snprintf(s, l, "%s%s", PA_USER_RUNTIME_PATH_PREFIX, pa_get_user_name(u, sizeof(u)));
    }
    

#ifdef OS_IS_WIN32
    {
        char buf[l];
        strcpy(buf, s);
        ExpandEnvironmentStrings(buf, s, l);
    }
#endif

    return s;
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
