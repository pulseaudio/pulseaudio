/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/un.h>
#include <locale.h>

#include <pulse/error.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>
#include <pulse/i18n.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/pid.h>

int main(int argc, char*argv[]) {

    pid_t pid ;
    int fd = -1;
    int ret = 1, i;
    struct sockaddr_un sa;
    char ibuf[PIPE_BUF], obuf[PIPE_BUF];
    size_t ibuf_index, ibuf_length, obuf_index, obuf_length;
    char *cli;
    pa_bool_t ibuf_eof, obuf_eof, ibuf_closed, obuf_closed;
    struct pollfd pollfd[3];
    struct pollfd *watch_socket, *watch_stdin, *watch_stdout;

    int stdin_type = 0, stdout_type = 0, fd_type = 0;

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, PULSE_LOCALEDIR);

    if (pa_pid_file_check_running(&pid, "pulseaudio") < 0) {
        pa_log(_("No PulseAudio daemon running, or not running as session daemon."));
        goto fail;
    }

    if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        pa_log(_("socket(PF_UNIX, SOCK_STREAM, 0): %s"), strerror(errno));
        goto fail;
    }

    pa_zero(sa);
    sa.sun_family = AF_UNIX;

    if (!(cli = pa_runtime_path("cli")))
        goto fail;

    pa_strlcpy(sa.sun_path, cli, sizeof(sa.sun_path));
    pa_xfree(cli);

    for (i = 0; i < 5; i++) {
        int r;

        if ((r = connect(fd, (struct sockaddr*) &sa, sizeof(sa))) < 0 && (errno != ECONNREFUSED && errno != ENOENT)) {
            pa_log(_("connect(): %s"), strerror(errno));
            goto fail;
        }

        if (r >= 0)
            break;

        if (pa_pid_file_kill(SIGUSR2, NULL, "pulseaudio") < 0) {
            pa_log(_("Failed to kill PulseAudio daemon."));
            goto fail;
        }

        pa_msleep(300);
    }

    if (i >= 5) {
        pa_log(_("Daemon not responding."));
        goto fail;
    }

    ibuf_index = ibuf_length = obuf_index = obuf_length = 0;
    ibuf_eof = obuf_eof = ibuf_closed = obuf_closed = FALSE;

    if (argc > 1) {
        for (i = 1; i < argc; i++) {
            size_t k;

            k = PA_MIN(sizeof(ibuf) - ibuf_length, strlen(argv[i]));
            memcpy(ibuf + ibuf_length, argv[i], k);
            ibuf_length += k;

            if (ibuf_length < sizeof(ibuf)) {
                ibuf[ibuf_length] = i < argc-1 ? ' ' : '\n';
                ibuf_length++;
            }
        }

        ibuf_eof = TRUE;
    }

    for (;;) {
        struct pollfd *p;

        if (ibuf_eof &&
            obuf_eof &&
            ibuf_length <= 0 &&
            obuf_length <= 0)
            break;

        if (ibuf_length <= 0 && ibuf_eof && !ibuf_closed) {
            shutdown(fd, SHUT_WR);
            ibuf_closed = TRUE;
        }

        if (obuf_length <= 0 && obuf_eof && !obuf_closed) {
            shutdown(fd, SHUT_RD);
            obuf_closed = TRUE;
        }

        pa_zero(pollfd);

        p = pollfd;

        if (ibuf_length > 0 || (!obuf_eof && obuf_length <= 0)) {
            watch_socket = p++;
            watch_socket->fd = fd;
            watch_socket->events =
                (ibuf_length > 0 ? POLLOUT : 0) |
                (!obuf_eof && obuf_length <= 0 ? POLLIN : 0);
        } else
            watch_socket = NULL;

        if (!ibuf_eof && ibuf_length <= 0) {
            watch_stdin = p++;
            watch_stdin->fd = STDIN_FILENO;
            watch_stdin->events = POLLIN;
        } else
            watch_stdin = NULL;

        if (obuf_length > 0) {
            watch_stdout = p++;
            watch_stdout->fd = STDOUT_FILENO;
            watch_stdout->events = POLLOUT;
        } else
            watch_stdout = NULL;

        if (poll(pollfd, p-pollfd, -1) < 0) {

            if (errno == EINTR)
                continue;

            pa_log(_("poll(): %s"), strerror(errno));
            goto fail;
        }

        if (watch_stdin) {
            if (watch_stdin->revents & POLLIN) {
                ssize_t r;
                pa_assert(ibuf_length <= 0);

                if ((r = pa_read(STDIN_FILENO, ibuf, sizeof(ibuf), &stdin_type)) <= 0) {
                    if (r < 0) {
                        pa_log(_("read(): %s"), strerror(errno));
                        goto fail;
                    }

                    ibuf_eof = TRUE;
                } else {
                    ibuf_length = (size_t) r;
                    ibuf_index = 0;
                }
            } else if (watch_stdin->revents & POLLHUP)
                ibuf_eof = TRUE;
        }

        if (watch_socket) {
            if (watch_socket->revents & POLLIN) {
                ssize_t r;
                pa_assert(obuf_length <= 0);

                if ((r = pa_read(fd, obuf, sizeof(obuf), &fd_type)) <= 0) {
                    if (r < 0) {
                        pa_log(_("read(): %s"), strerror(errno));
                        goto fail;
                    }

                    obuf_eof = TRUE;
                } else {
                    obuf_length = (size_t) r;
                    obuf_index = 0;
                }
            } else if (watch_socket->revents & POLLHUP)
                obuf_eof = TRUE;
        }

        if (watch_stdout) {
            if (watch_stdout->revents & POLLHUP) {
                obuf_eof = TRUE;
                obuf_length = 0;
            } else if (watch_stdout->revents & POLLOUT) {
                ssize_t r;
                pa_assert(obuf_length > 0);

                if ((r = pa_write(STDOUT_FILENO, obuf + obuf_index, obuf_length, &stdout_type)) < 0) {
                    pa_log(_("write(): %s"), strerror(errno));
                    goto fail;
                }

                obuf_length -= (size_t) r;
                obuf_index += obuf_index;
            }
        }

        if (watch_socket) {
            if (watch_socket->revents & POLLHUP) {
                ibuf_eof = TRUE;
                ibuf_length = 0;
            } if (watch_socket->revents & POLLOUT) {
                ssize_t r;
                pa_assert(ibuf_length > 0);

                if ((r = pa_write(fd, ibuf + ibuf_index, ibuf_length, &fd_type)) < 0) {
                    pa_log(_("write(): %s"), strerror(errno));
                    goto fail;
                }

                ibuf_length -= (size_t) r;
                ibuf_index += obuf_index;
            }
        }
    }

    ret = 0;

fail:
    if (fd >= 0)
        pa_close(fd);

    return ret;
}
