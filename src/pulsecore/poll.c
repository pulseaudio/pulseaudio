
/***
  This file is part of PulseAudio.

  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

/***
   Based on work for the GNU C Library.
   Copyright (C) 1994, 1996, 1997 Free Software Foundation, Inc.
***/

/* Poll the file descriptors described by the NFDS structures starting at
   FDS.  If TIMEOUT is nonzero and not -1, allow TIMEOUT milliseconds for
   an event to occur; if TIMEOUT is -1, block until an event occurs.
   Returns the number of file descriptors with events, zero if timed out,
   or -1 for errors.  */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "winsock.h"

#ifndef HAVE_POLL_H

#include <pulsecore/core-util.h>

#include "poll.h"

int poll (struct pollfd *fds, unsigned long int nfds, int timeout) {
    struct timeval tv;
    fd_set rset, wset, xset;
    struct pollfd *f;
    int ready;
    int maxfd = 0;
    char data[64];

    FD_ZERO (&rset);
    FD_ZERO (&wset);
    FD_ZERO (&xset);

    if (nfds == 0) {
        if (timeout >= 0) {
            pa_msleep(timeout);
            return 0;
        }

#ifdef OS_IS_WIN32
        /*
         * Windows does not support signals properly so waiting for them would
         * mean a deadlock.
         */
        pa_msleep(100);
        return 0;
#else
        return select(0, NULL, NULL, NULL, NULL);
#endif
    }

    for (f = fds; f < &fds[nfds]; ++f) {
        if (f->fd != -1) {
            if (f->events & POLLIN)
                FD_SET (f->fd, &rset);
            if (f->events & POLLOUT)
                FD_SET (f->fd, &wset);
            if (f->events & POLLPRI)
                FD_SET (f->fd, &xset);
            if (f->fd > maxfd && (f->events & (POLLIN|POLLOUT|POLLPRI)))
                maxfd = f->fd;
        }
    }

    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;

    ready = select ((SELECT_TYPE_ARG1) maxfd + 1, SELECT_TYPE_ARG234 &rset,
                    SELECT_TYPE_ARG234 &wset, SELECT_TYPE_ARG234 &xset,
                    SELECT_TYPE_ARG5 (timeout == -1 ? NULL : &tv));
    if ((ready == -1) && (errno == EBADF)) {
        ready = 0;

        FD_ZERO (&rset);
        FD_ZERO (&wset);
        FD_ZERO (&xset);

        maxfd = -1;

        for (f = fds; f < &fds[nfds]; ++f) {
            if (f->fd != -1) {
                fd_set sngl_rset, sngl_wset, sngl_xset;

                FD_ZERO (&sngl_rset);
                FD_ZERO (&sngl_wset);
                FD_ZERO (&sngl_xset);

                if (f->events & POLLIN)
                    FD_SET (f->fd, &sngl_rset);
                if (f->events & POLLOUT)
                    FD_SET (f->fd, &sngl_wset);
                if (f->events & POLLPRI)
                    FD_SET (f->fd, &sngl_xset);
                if (f->events & (POLLIN|POLLOUT|POLLPRI)) {
                    struct timeval singl_tv;

                    singl_tv.tv_sec = 0;
                    singl_tv.tv_usec = 0;

                    if (select((SELECT_TYPE_ARG1) f->fd, SELECT_TYPE_ARG234 &rset,
                               SELECT_TYPE_ARG234 &wset, SELECT_TYPE_ARG234 &xset,
                               SELECT_TYPE_ARG5 &singl_tv) != -1) {
                        if (f->events & POLLIN)
                            FD_SET (f->fd, &rset);
                        if (f->events & POLLOUT)
                            FD_SET (f->fd, &wset);
                        if (f->events & POLLPRI)
                            FD_SET (f->fd, &xset);
                        if (f->fd > maxfd && (f->events & (POLLIN|POLLOUT|POLLPRI)))
                            maxfd = f->fd;
                        ++ready;
                    } else if (errno == EBADF)
                        f->revents |= POLLNVAL;
                }
            }
        }

        if (ready) {
        /* Linux alters the tv struct... but it shouldn't matter here ...
         * as we're going to be a little bit out anyway as we've just eaten
         * more than a couple of cpu cycles above */
            ready = select ((SELECT_TYPE_ARG1) maxfd + 1, SELECT_TYPE_ARG234 &rset,
                            SELECT_TYPE_ARG234 &wset, SELECT_TYPE_ARG234 &xset,
                            SELECT_TYPE_ARG5 (timeout == -1 ? NULL : &tv));
        }
    }

#ifdef OS_IS_WIN32
    errno = WSAGetLastError();
#endif

    if (ready > 0) {
        ready = 0;
        for (f = fds; f < &fds[nfds]; ++f) {
            f->revents = 0;
            if (f->fd != -1) {
                if (FD_ISSET (f->fd, &rset)) {
                    /* support for POLLHUP.  An hung up descriptor does not
                       increase the return value! */
                    if (recv (f->fd, data, 64, MSG_PEEK) == -1) {
                        if (errno == ESHUTDOWN || errno == ECONNRESET ||
                            errno == ECONNABORTED || errno == ENETRESET) {
                            fprintf(stderr, "Hangup\n");
                            f->revents |= POLLHUP;
                        }
                    }

                    if (f->revents == 0)
                        f->revents |= POLLIN;
                }
                if (FD_ISSET (f->fd, &wset))
                    f->revents |= POLLOUT;
                if (FD_ISSET (f->fd, &xset))
                    f->revents |= POLLPRI;
            }
            if (f->revents)
                ready++;
        }
    }

    return ready;
}

#endif /* HAVE_SYS_POLL_H */
