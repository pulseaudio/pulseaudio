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

#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "winsock.h"

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/log.h>

#include "iochannel.h"

struct pa_iochannel {
    int ifd, ofd;
    int ifd_type, ofd_type;
    pa_mainloop_api* mainloop;

    pa_iochannel_cb_t callback;
    void*userdata;
    
    int readable;
    int writable;
    int hungup;
    
    int no_close;

    pa_io_event* input_event, *output_event;
};

static void enable_mainloop_sources(pa_iochannel *io) {
    assert(io);

    if (io->input_event == io->output_event && io->input_event) {
        pa_io_event_flags_t f = PA_IO_EVENT_NULL;
        assert(io->input_event);
        
        if (!io->readable)
            f |= PA_IO_EVENT_INPUT;
        if (!io->writable)
            f |= PA_IO_EVENT_OUTPUT;

        io->mainloop->io_enable(io->input_event, f);
    } else {
        if (io->input_event)
            io->mainloop->io_enable(io->input_event, io->readable ? PA_IO_EVENT_NULL : PA_IO_EVENT_INPUT);
        if (io->output_event)
            io->mainloop->io_enable(io->output_event, io->writable ? PA_IO_EVENT_NULL : PA_IO_EVENT_OUTPUT);
    }
}

static void callback(pa_mainloop_api* m, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata) {
    pa_iochannel *io = userdata;
    int changed = 0;
    
    assert(m);
    assert(e);
    assert(fd >= 0);
    assert(userdata);

    if ((f & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) && !io->hungup) {
        io->hungup = 1;
        changed = 1;
    }

    if ((f & PA_IO_EVENT_INPUT) && !io->readable) {
        io->readable = 1;
        changed = 1;
        assert(e == io->input_event);
    }
    
    if ((f & PA_IO_EVENT_OUTPUT) && !io->writable) {
        io->writable = 1;
        changed = 1;
        assert(e == io->output_event);
    }

    if (changed) {
        enable_mainloop_sources(io);
        
        if (io->callback)
            io->callback(io, io->userdata);
    }
}

pa_iochannel* pa_iochannel_new(pa_mainloop_api*m, int ifd, int ofd) {
    pa_iochannel *io;
    
    assert(m);
    assert(ifd >= 0 || ofd >= 0);

    io = pa_xnew(pa_iochannel, 1);
    io->ifd = ifd;
    io->ofd = ofd;
    io->ifd_type = io->ofd_type = 0;
    io->mainloop = m;

    io->userdata = NULL;
    io->callback = NULL;
    io->readable = 0;
    io->writable = 0;
    io->hungup = 0;
    io->no_close = 0;

    io->input_event = io->output_event = NULL;

    if (ifd == ofd) {
        assert(ifd >= 0);
        pa_make_nonblock_fd(io->ifd);
        io->input_event = io->output_event = m->io_new(m, ifd, PA_IO_EVENT_INPUT|PA_IO_EVENT_OUTPUT, callback, io);
    } else {

        if (ifd >= 0) {
            pa_make_nonblock_fd(io->ifd);
            io->input_event = m->io_new(m, ifd, PA_IO_EVENT_INPUT, callback, io);
        }

        if (ofd >= 0) {
            pa_make_nonblock_fd(io->ofd);
            io->output_event = m->io_new(m, ofd, PA_IO_EVENT_OUTPUT, callback, io);
        }
    }

    return io;
}

void pa_iochannel_free(pa_iochannel*io) {
    assert(io);

    if (io->input_event)
        io->mainloop->io_free(io->input_event);
    
    if (io->output_event && (io->output_event != io->input_event))
        io->mainloop->io_free(io->output_event);

    if (!io->no_close) {
        if (io->ifd >= 0)
            
            close(io->ifd);
        if (io->ofd >= 0 && io->ofd != io->ifd)
            close(io->ofd);
    }
    
    pa_xfree(io);
}

int pa_iochannel_is_readable(pa_iochannel*io) {
    assert(io);
    
    return io->readable || io->hungup;
}

int pa_iochannel_is_writable(pa_iochannel*io) {
    assert(io);
    
    return io->writable && !io->hungup;
}

int pa_iochannel_is_hungup(pa_iochannel*io) {
    assert(io);
    
    return io->hungup;
}

ssize_t pa_iochannel_write(pa_iochannel*io, const void*data, size_t l) {
    ssize_t r;
    
    assert(io);
    assert(data);
    assert(l);
    assert(io->ofd >= 0);

    r = pa_write(io->ofd, data, l, &io->ofd_type);
    if (r >= 0) {
        io->writable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

ssize_t pa_iochannel_read(pa_iochannel*io, void*data, size_t l) {
    ssize_t r;
    
    assert(io);
    assert(data);
    assert(io->ifd >= 0);

    r = pa_read(io->ifd, data, l, &io->ifd_type);
    if (r >= 0) {
        io->readable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

#ifdef SCM_CREDENTIALS

int pa_iochannel_creds_supported(pa_iochannel *io) {
    struct sockaddr_un sa;
    socklen_t l;
    
    assert(io);
    assert(io->ifd >= 0);
    assert(io->ofd == io->ifd);

    l = sizeof(sa);
    
    if (getsockname(io->ifd, (struct sockaddr*) &sa, &l) < 0)
        return 0;

    return sa.sun_family == AF_UNIX;
}

int pa_iochannel_creds_enable(pa_iochannel *io) {
    int t = 1;

    assert(io);
    assert(io->ifd >= 0);
    
    if (setsockopt(io->ifd, SOL_SOCKET, SO_PASSCRED, &t, sizeof(t)) < 0) {
        pa_log_error("setsockopt(SOL_SOCKET, SO_PASSCRED): %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

ssize_t pa_iochannel_write_with_creds(pa_iochannel*io, const void*data, size_t l) {
    ssize_t r;
    struct msghdr mh;
    struct iovec iov;
    uint8_t cmsg_data[CMSG_SPACE(sizeof(struct ucred))];
    struct ucred *ucred;
    struct cmsghdr *cmsg;
    
    assert(io);
    assert(data);
    assert(l);
    assert(io->ofd >= 0);

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = (void*) data;
    iov.iov_len = l;

    memset(cmsg_data, 0, sizeof(cmsg_data));
    cmsg = (struct cmsghdr*)  cmsg_data;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;

    ucred = (struct ucred*) CMSG_DATA(cmsg);
    ucred->pid = getpid();
    ucred->uid = getuid();
    ucred->gid = getgid();
    
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = NULL;
    mh.msg_namelen = 0;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsg_data;
    mh.msg_controllen = sizeof(cmsg_data);
    mh.msg_flags = 0;

    if ((r = sendmsg(io->ofd, &mh, MSG_NOSIGNAL)) >= 0) {
        io->writable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

ssize_t pa_iochannel_read_with_creds(pa_iochannel*io, void*data, size_t l, struct ucred *ucred, int *creds_valid) {
    ssize_t r;
    struct msghdr mh;
    struct iovec iov;
    uint8_t cmsg_data[CMSG_SPACE(sizeof(struct ucred))];
    
    assert(io);
    assert(data);
    assert(l);
    assert(io->ifd >= 0);
    assert(ucred);
    assert(creds_valid);

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = data;
    iov.iov_len = l;

    memset(cmsg_data, 0, sizeof(cmsg_data));

    memset(&mh, 0, sizeof(mh));
    mh.msg_name = NULL;
    mh.msg_namelen = 0;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsg_data;
    mh.msg_controllen = sizeof(cmsg_data);
    mh.msg_flags = 0;

    if ((r = recvmsg(io->ifd, &mh, 0)) >= 0) {
        struct cmsghdr *cmsg;

        *creds_valid = 0;
    
        for (cmsg = CMSG_FIRSTHDR(&mh); cmsg; cmsg = CMSG_NXTHDR(&mh, cmsg)) {
            
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
                assert(cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred)));
                memcpy(ucred, CMSG_DATA(cmsg), sizeof(struct ucred));
                *creds_valid = 1;
                break;
            }
        }

        io->readable = 0;
        enable_mainloop_sources(io);
    }
    
    return r;
}
#else /* SCM_CREDENTIALS */

int pa_iochannel_creds_supported(pa_iochannel *io) {
    return 0;
}

int pa_iochannel_creds_enable(pa_iochannel *io) {
    return -1;
}

ssize_t pa_iochannel_write_with_creds(pa_iochannel*io, const void*data, size_t l) {
    pa_log_error("pa_iochannel_write_with_creds() not supported.");
    return -1;
}

ssize_t pa_iochannel_read_with_creds(pa_iochannel*io, void*data, size_t l, struct ucred *ucred, int *creds_valid) {
    pa_log_error("pa_iochannel_read_with_creds() not supported.");
    return -1;
}

#endif /* SCM_CREDENTIALS */

void pa_iochannel_set_callback(pa_iochannel*io, pa_iochannel_cb_t _callback, void *userdata) {
    assert(io);
    
    io->callback = _callback;
    io->userdata = userdata;
}

void pa_iochannel_set_noclose(pa_iochannel*io, int b) {
    assert(io);
    
    io->no_close = b;
}

void pa_iochannel_socket_peer_to_string(pa_iochannel*io, char*s, size_t l) {
    assert(io);
    assert(s);
    assert(l);
    
    pa_socket_peer_to_string(io->ifd, s, l);
}

int pa_iochannel_socket_set_rcvbuf(pa_iochannel *io, size_t l) {
    assert(io);
    
    return pa_socket_set_rcvbuf(io->ifd, l);
}

int pa_iochannel_socket_set_sndbuf(pa_iochannel *io, size_t l) {
    assert(io);
    
    return pa_socket_set_sndbuf(io->ofd, l);
}

pa_mainloop_api* pa_iochannel_get_mainloop_api(pa_iochannel *io) {
    assert(io);
    
    return io->mainloop;
}
