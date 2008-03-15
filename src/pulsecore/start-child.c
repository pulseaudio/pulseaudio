/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

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

#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>

#include "start-child.h"

int pa_start_child_for_read(const char *name, const char *argv1, pid_t *pid) {
    pid_t child;
    int pipe_fds[2] = { -1, -1 };

    if (pipe(pipe_fds) < 0) {
        pa_log("pipe() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if ((child = fork()) == (pid_t) -1) {
        pa_log("fork() failed: %s", pa_cstrerror(errno));
        goto fail;

    } else if (child != 0) {

        /* Parent */
        pa_assert_se(pa_close(pipe_fds[1]) == 0);

        if (pid)
            *pid = child;

        return pipe_fds[0];
    } else {
#ifdef __linux__
        DIR* d;
#endif
        int max_fd, i;

        /* child */

        pa_reset_priority();

        pa_assert_se(pa_close(pipe_fds[0]) == 0);
        pa_assert_se(dup2(pipe_fds[1], 1) == 1);

        if (pipe_fds[1] != 1)
            pa_assert_se(pa_close(pipe_fds[1]) == 0);

        pa_close(0);
        pa_assert_se(open("/dev/null", O_RDONLY) == 0);

        pa_close(2);
        pa_assert_se(open("/dev/null", O_WRONLY) == 2);

#ifdef __linux__

        if ((d = opendir("/proc/self/fd/"))) {

            struct dirent *de;

            while ((de = readdir(d))) {
                char *e = NULL;
                int fd;

                if (de->d_name[0] == '.')
                    continue;

                errno = 0;
                fd = strtol(de->d_name, &e, 10);
                pa_assert(errno == 0 && e && *e == 0);

                if (fd >= 3 && dirfd(d) != fd)
                    pa_close(fd);
            }

            closedir(d);
        } else {

#endif

            max_fd = 1024;

#ifdef HAVE_SYS_RESOURCE_H
            {
                struct rlimit r;
                if (getrlimit(RLIMIT_NOFILE, &r) == 0)
                    max_fd = r.rlim_max;
            }
#endif

            for (i = 3; i < max_fd; i++)
                pa_close(i);

#ifdef __linux__
        }
#endif

#ifdef PR_SET_PDEATHSIG
        /* On Linux we can use PR_SET_PDEATHSIG to have the helper
        process killed when the daemon dies abnormally. On non-Linux
        machines the client will die as soon as it writes data to
        stdout again (SIGPIPE) */

        prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
#endif

#ifdef SIGPIPE
        /* Make sure that SIGPIPE kills the child process */
        signal(SIGPIPE, SIG_DFL);
#endif

#ifdef SIGTERM
        /* Make sure that SIGTERM kills the child process */
        signal(SIGTERM, SIG_DFL);
#endif

        execl(name, name, argv1, NULL);
        _exit(1);
    }

fail:
    pa_close_pipe(pipe_fds);

    return -1;
}
