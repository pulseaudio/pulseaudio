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

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <signal.h>

#include "pid.h"
#include "util.h"
#include "log.h"

static pid_t read_pid(const char *fn, int fd) {
    ssize_t r;
    char t[20], *e = NULL;
    long int pid;

    assert(fn && fd >= 0);

    if ((r = pa_loop_read(fd, t, sizeof(t)-1)) < 0) {
        pa_log(__FILE__": WARNING: failed to read PID file '%s': %s\n", fn, strerror(errno));
        return (pid_t) -1;
    }

    if (r == 0)
        return (pid_t) 0;
    
    t[r] = 0;

    if (!t[0] || (pid = strtol(t, &e, 0)) == 0 || (*e != 0 && *e != '\n')) {
        pa_log(__FILE__": WARNING: failed to parse PID file '%s'\n", fn);
        return (pid_t) -1;
    }

    return (pid_t) pid;
}

int pa_pid_file_create(void) {
    int fd = -1, lock = -1;
    int ret = -1;
    char fn[PATH_MAX];
    char t[20];
    pid_t pid;
    size_t l;

    pa_runtime_path("pid", fn, sizeof(fn));
    if ((fd = open(fn, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR)) < 0) {
        pa_log(__FILE__": WARNING: failed to open PID file '%s': %s\n", fn, strerror(errno));
        goto fail;
    }

    lock = pa_lock_fd(fd, 1);

    if ((pid = read_pid(fn, fd)) == (pid_t) -1)
        pa_log(__FILE__": corrupt PID file, overwriting.\n");
    else if (pid > 0) {
        if (kill(pid, 0) >= 0 || errno != ESRCH) {
            pa_log(__FILE__": valid PID file.\n");
            goto fail;
        }

        pa_log(__FILE__": stale PID file, overwriting.\n");
    }

    if (lseek(fd, 0, SEEK_SET) == (off_t) -1 || ftruncate(fd, 0) < 0) {
        pa_log(__FILE__": failed to truncate PID fil: %s.\n", strerror(errno));
        goto fail;
    }
    
    snprintf(t, sizeof(t), "%lu\n", (unsigned long) getpid());
    l = strlen(t);
    
    if (pa_loop_write(fd, t, l) != (ssize_t) l) {
        pa_log(__FILE__": failed to write PID file.\n");
        goto fail;
    }

    ret = 0;
    
fail:
    if (fd >= 0) {
        if (lock >= 0)
            pa_lock_fd(fd, 0);

        close(fd);
    }
    
    return ret;
}

int pa_pid_file_remove(void) {
    int fd = -1, lock = -1;
    char fn[PATH_MAX];
    int ret = -1;
    pid_t pid;

    pa_runtime_path("pid", fn, sizeof(fn));
    if ((fd = open(fn, O_RDWR)) < 0) {
        pa_log(__FILE__": WARNING: failed to open PID file '%s': %s\n", fn, strerror(errno));
        goto fail;
    }

    lock = pa_lock_fd(fd, 1);

    if ((pid = read_pid(fn, fd)) == (pid_t) -1)
        goto fail;

    if (pid != getpid()) {
        pa_log(__FILE__": WARNING: PID file '%s' not mine!\n", fn);
        goto fail;
    }

    if (ftruncate(fd, 0) < 0) {
        pa_log(__FILE__": failed to truncate PID file '%s': %s\n", fn, strerror(errno));
        goto fail;
    }

    if (unlink(fn) < 0) {
        pa_log(__FILE__": failed to remove PID file '%s': %s\n", fn, strerror(errno));
        goto fail;
    }

    ret = 0;
    
fail:

    if (fd >= 0) {
        if (lock >= 0)
            pa_lock_fd(fd, 0);

        close(fd);
    }

    return ret;
}

int pa_pid_file_check_running(pid_t *pid) {
    return pa_pid_file_kill(0, pid);
}

int pa_pid_file_kill(int sig, pid_t *pid) {
    int fd = -1, lock = -1;
    char fn[PATH_MAX];
    int ret = -1;
    pid_t _pid;

    if (!pid)
        pid = &_pid;

    pa_runtime_path("pid", fn, sizeof(fn));
    if ((fd = open(fn, O_RDONLY)) < 0)
        goto fail;

    lock = pa_lock_fd(fd, 1);

    if ((*pid = read_pid(fn, fd)) == (pid_t) -1)
        goto fail;

    ret = kill(*pid, sig);
    
fail:

    if (fd >= 0) {
        if (lock >= 0)
            pa_lock_fd(fd, 0);

        close(fd);
    }

    return ret;
    
}
