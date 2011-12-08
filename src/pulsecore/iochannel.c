/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006-2007 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/socket.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "iochannel.h"

struct pa_iochannel {
    int ifd, ofd;
    int ifd_type, ofd_type;
    pa_mainloop_api* mainloop;

    pa_iochannel_cb_t callback;
    void*userdata;

    pa_bool_t readable:1;
    pa_bool_t writable:1;
    pa_bool_t hungup:1;
    pa_bool_t no_close:1;

    pa_io_event* input_event, *output_event;
};

static void callback(pa_mainloop_api* m, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata);

static void delete_events(pa_iochannel *io) {
    pa_assert(io);

    if (io->input_event)
        io->mainloop->io_free(io->input_event);

    if (io->output_event && io->output_event != io->input_event)
        io->mainloop->io_free(io->output_event);

    io->input_event = io->output_event = NULL;
}

static void enable_events(pa_iochannel *io) {
    pa_assert(io);

    if (io->hungup) {
        delete_events(io);
        return;
    }

    if (io->ifd == io->ofd && io->ifd >= 0) {
        pa_io_event_flags_t f = PA_IO_EVENT_NULL;

        if (!io->readable)
            f |= PA_IO_EVENT_INPUT;
        if (!io->writable)
            f |= PA_IO_EVENT_OUTPUT;

        pa_assert(io->input_event == io->output_event);

        if (f != PA_IO_EVENT_NULL) {
            if (io->input_event)
                io->mainloop->io_enable(io->input_event, f);
            else
                io->input_event = io->output_event = io->mainloop->io_new(io->mainloop, io->ifd, f, callback, io);
        } else
            delete_events(io);

    } else {

        if (io->ifd >= 0) {
            if (!io->readable) {
                if (io->input_event)
                    io->mainloop->io_enable(io->input_event, PA_IO_EVENT_INPUT);
                else
                    io->input_event = io->mainloop->io_new(io->mainloop, io->ifd, PA_IO_EVENT_INPUT, callback, io);
            } else if (io->input_event) {
                io->mainloop->io_free(io->input_event);
                io->input_event = NULL;
            }
        }

        if (io->ofd >= 0) {
            if (!io->writable) {
                if (io->output_event)
                    io->mainloop->io_enable(io->output_event, PA_IO_EVENT_OUTPUT);
                else
                    io->output_event = io->mainloop->io_new(io->mainloop, io->ofd, PA_IO_EVENT_OUTPUT, callback, io);
            } else if (io->input_event) {
                io->mainloop->io_free(io->output_event);
                io->output_event = NULL;
            }
        }
    }
}

static void callback(pa_mainloop_api* m, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata) {
    pa_iochannel *io = userdata;
    pa_bool_t changed = FALSE;

    pa_assert(m);
    pa_assert(e);
    pa_assert(fd >= 0);
    pa_assert(userdata);

    if ((f & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) && !io->hungup) {
        io->hungup = TRUE;
        changed = TRUE;
    }

    if ((f & PA_IO_EVENT_INPUT) && !io->readable) {
        io->readable = TRUE;
        changed = TRUE;
        pa_assert(e == io->input_event);
    }

    if ((f & PA_IO_EVENT_OUTPUT) && !io->writable) {
        io->writable = TRUE;
        changed = TRUE;
        pa_assert(e == io->output_event);
    }

    if (changed) {
        enable_events(io);

        if (io->callback)
            io->callback(io, io->userdata);
    }
}

pa_iochannel* pa_iochannel_new(pa_mainloop_api*m, int ifd, int ofd) {
    pa_iochannel *io;

    pa_assert(m);
    pa_assert(ifd >= 0 || ofd >= 0);

    io = pa_xnew0(pa_iochannel, 1);
    io->ifd = ifd;
    io->ofd = ofd;
    io->mainloop = m;

    if (io->ifd >= 0)
        pa_make_fd_nonblock(io->ifd);

    if (io->ofd >= 0 && io->ofd != io->ifd)
        pa_make_fd_nonblock(io->ofd);

    enable_events(io);
    return io;
}

void pa_iochannel_free(pa_iochannel*io) {
    pa_assert(io);

    delete_events(io);

    if (!io->no_close) {
        if (io->ifd >= 0)
            pa_close(io->ifd);
        if (io->ofd >= 0 && io->ofd != io->ifd)
            pa_close(io->ofd);
    }

    pa_xfree(io);
}

pa_bool_t pa_iochannel_is_readable(pa_iochannel*io) {
    pa_assert(io);

    return io->readable || io->hungup;
}

pa_bool_t pa_iochannel_is_writable(pa_iochannel*io) {
    pa_assert(io);

    return io->writable && !io->hungup;
}

pa_bool_t pa_iochannel_is_hungup(pa_iochannel*io) {
    pa_assert(io);

    return io->hungup;
}

ssize_t pa_iochannel_write(pa_iochannel*io, const void*data, size_t l) {
    ssize_t r;

    pa_assert(io);
    pa_assert(data);
    pa_assert(l);
    pa_assert(io->ofd >= 0);

    if ((r = pa_write(io->ofd, data, l, &io->ofd_type)) >= 0) {
        io->writable = io->hungup = FALSE;
        enable_events(io);
    }

    return r;
}

ssize_t pa_iochannel_read(pa_iochannel*io, void*data, size_t l) {
    ssize_t r;

    pa_assert(io);
    pa_assert(data);
    pa_assert(io->ifd >= 0);

    if ((r = pa_read(io->ifd, data, l, &io->ifd_type)) >= 0) {

        /* We also reset the hangup flag here to ensure that another
         * IO callback is triggered so that we will again call into
         * user code */
        io->readable = io->hungup = FALSE;
        enable_events(io);
    }

    return r;
}

#ifdef HAVE_CREDS

pa_bool_t pa_iochannel_creds_supported(pa_iochannel *io) {
    struct {
        struct sockaddr sa;
#ifdef HAVE_SYS_UN_H
        struct sockaddr_un un;
#endif
        struct sockaddr_storage storage;
    } sa;

    socklen_t l;

    pa_assert(io);
    pa_assert(io->ifd >= 0);
    pa_assert(io->ofd == io->ifd);

    l = sizeof(sa);
    if (getsockname(io->ifd, &sa.sa, &l) < 0)
        return FALSE;

    return sa.sa.sa_family == AF_UNIX;
}

int pa_iochannel_creds_enable(pa_iochannel *io) {
    int t = 1;

    pa_assert(io);
    pa_assert(io->ifd >= 0);

    if (setsockopt(io->ifd, SOL_SOCKET, SO_PASSCRED, &t, sizeof(t)) < 0) {
        pa_log_error("setsockopt(SOL_SOCKET, SO_PASSCRED): %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

ssize_t pa_iochannel_write_with_creds(pa_iochannel*io, const void*data, size_t l, const pa_creds *ucred) {
    ssize_t r;
    struct msghdr mh;
    struct iovec iov;
    union {
        struct cmsghdr hdr;
        uint8_t data[CMSG_SPACE(sizeof(struct ucred))];
    } cmsg;
    struct ucred *u;

    pa_assert(io);
    pa_assert(data);
    pa_assert(l);
    pa_assert(io->ofd >= 0);

    pa_zero(iov);
    iov.iov_base = (void*) data;
    iov.iov_len = l;

    pa_zero(cmsg);
    cmsg.hdr.cmsg_len = CMSG_LEN(sizeof(struct ucred));
    cmsg.hdr.cmsg_level = SOL_SOCKET;
    cmsg.hdr.cmsg_type = SCM_CREDENTIALS;

    u = (struct ucred*) CMSG_DATA(&cmsg.hdr);

    u->pid = getpid();
    if (ucred) {
        u->uid = ucred->uid;
        u->gid = ucred->gid;
    } else {
        u->uid = getuid();
        u->gid = getgid();
    }

    pa_zero(mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = &cmsg;
    mh.msg_controllen = sizeof(cmsg);

    if ((r = sendmsg(io->ofd, &mh, MSG_NOSIGNAL)) >= 0) {
        io->writable = io->hungup = FALSE;
        enable_events(io);
    }

    return r;
}

ssize_t pa_iochannel_read_with_creds(pa_iochannel*io, void*data, size_t l, pa_creds *creds, pa_bool_t *creds_valid) {
    ssize_t r;
    struct msghdr mh;
    struct iovec iov;
    union {
        struct cmsghdr hdr;
        uint8_t data[CMSG_SPACE(sizeof(struct ucred))];
    } cmsg;

    pa_assert(io);
    pa_assert(data);
    pa_assert(l);
    pa_assert(io->ifd >= 0);
    pa_assert(creds);
    pa_assert(creds_valid);

    pa_zero(iov);
    iov.iov_base = data;
    iov.iov_len = l;

    pa_zero(cmsg);
    pa_zero(mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = &cmsg;
    mh.msg_controllen = sizeof(cmsg);

    if ((r = recvmsg(io->ifd, &mh, 0)) >= 0) {
        struct cmsghdr *cmh;

        *creds_valid = FALSE;

        for (cmh = CMSG_FIRSTHDR(&mh); cmh; cmh = CMSG_NXTHDR(&mh, cmh)) {

            if (cmh->cmsg_level == SOL_SOCKET && cmh->cmsg_type == SCM_CREDENTIALS) {
                struct ucred u;
                pa_assert(cmh->cmsg_len == CMSG_LEN(sizeof(struct ucred)));
                memcpy(&u, CMSG_DATA(cmh), sizeof(struct ucred));

                creds->gid = u.gid;
                creds->uid = u.uid;
                *creds_valid = TRUE;
                break;
            }
        }

        io->readable = io->hungup = FALSE;
        enable_events(io);
    }

    return r;
}

#endif /* HAVE_CREDS */

void pa_iochannel_set_callback(pa_iochannel*io, pa_iochannel_cb_t _callback, void *userdata) {
    pa_assert(io);

    io->callback = _callback;
    io->userdata = userdata;
}

void pa_iochannel_set_noclose(pa_iochannel*io, pa_bool_t b) {
    pa_assert(io);

    io->no_close = !!b;
}

void pa_iochannel_socket_peer_to_string(pa_iochannel*io, char*s, size_t l) {
    pa_assert(io);
    pa_assert(s);
    pa_assert(l);

    pa_socket_peer_to_string(io->ifd, s, l);
}

int pa_iochannel_socket_set_rcvbuf(pa_iochannel *io, size_t l) {
    pa_assert(io);

    return pa_socket_set_rcvbuf(io->ifd, l);
}

int pa_iochannel_socket_set_sndbuf(pa_iochannel *io, size_t l) {
    pa_assert(io);

    return pa_socket_set_sndbuf(io->ofd, l);
}

pa_mainloop_api* pa_iochannel_get_mainloop_api(pa_iochannel *io) {
    pa_assert(io);

    return io->mainloop;
}

int pa_iochannel_get_recv_fd(pa_iochannel *io) {
    pa_assert(io);

    return io->ifd;
}

int pa_iochannel_get_send_fd(pa_iochannel *io) {
    pa_assert(io);

    return io->ofd;
}

pa_bool_t pa_iochannel_socket_is_local(pa_iochannel *io) {
    pa_assert(io);

    if (pa_socket_is_local(io->ifd))
        return TRUE;

    if (io->ifd != io->ofd)
        if (pa_socket_is_local(io->ofd))
            return TRUE;

    return FALSE;
}
