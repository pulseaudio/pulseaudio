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

#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "winsock.h"

#include "iochannel.h"
#include "util.h"
#include "socket-util.h"
#include "xmalloc.h"

struct pa_iochannel {
    int ifd, ofd;
    struct pa_mainloop_api* mainloop;

    void (*callback)(struct pa_iochannel*io, void *userdata);
    void*userdata;
    
    int readable;
    int writable;
    int hungup;
    
    int no_close;

    struct pa_io_event* input_event, *output_event;
};

static void enable_mainloop_sources(struct pa_iochannel *io) {
    assert(io);

    if (io->input_event == io->output_event && io->input_event) {
        enum pa_io_event_flags f = PA_IO_EVENT_NULL;
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

static void callback(struct pa_mainloop_api* m, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct pa_iochannel *io = userdata;
    int changed = 0;
    assert(m && e && fd >= 0 && userdata);

    if ((f & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) && !io->hungup) {
        io->hungup = 1;
        changed = 1;

        if (e == io->input_event) {
            io->mainloop->io_free(io->input_event);
            io->input_event = NULL;

            if (io->output_event == e)
                io->output_event = NULL;
        }

        if (e == io->output_event) {
            io->mainloop->io_free(io->output_event);
            io->output_event = NULL;
        }
    } else {

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
    }

    if (changed) {
        enable_mainloop_sources(io);
        
        if (io->callback)
            io->callback(io, io->userdata);
    }
}

struct pa_iochannel* pa_iochannel_new(struct pa_mainloop_api*m, int ifd, int ofd) {
    struct pa_iochannel *io;
    assert(m && (ifd >= 0 || ofd >= 0));

    io = pa_xmalloc(sizeof(struct pa_iochannel));
    io->ifd = ifd;
    io->ofd = ofd;
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

void pa_iochannel_free(struct pa_iochannel*io) {
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

int pa_iochannel_is_readable(struct pa_iochannel*io) {
    assert(io);
    return io->readable || io->hungup;
}

int pa_iochannel_is_writable(struct pa_iochannel*io) {
    assert(io);
    return io->writable && !io->hungup;
}

int pa_iochannel_is_hungup(struct pa_iochannel*io) {
    assert(io);
    return io->hungup;
}

ssize_t pa_iochannel_write(struct pa_iochannel*io, const void*data, size_t l) {
    ssize_t r;
    assert(io && data && l && io->ofd >= 0);

#ifdef OS_IS_WIN32
    r = send(io->ofd, data, l, 0);
    if (r < 0) {
        if (WSAGetLastError() != WSAENOTSOCK) {
            errno = WSAGetLastError();
            return r;
        }
    }

    if (r < 0)
#endif
        r = write(io->ofd, data, l);
    if (r >= 0) {
        io->writable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

ssize_t pa_iochannel_read(struct pa_iochannel*io, void*data, size_t l) {
    ssize_t r;
    assert(io && data && io->ifd >= 0);
    
#ifdef OS_IS_WIN32
    r = recv(io->ifd, data, l, 0);
    if (r < 0) {
        if (WSAGetLastError() != WSAENOTSOCK) {
            errno = WSAGetLastError();
            return r;
        }
    }

    if (r < 0)
#endif
        r = read(io->ifd, data, l);
    if (r >= 0) {
        io->readable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

void pa_iochannel_set_callback(struct pa_iochannel*io, void (*callback)(struct pa_iochannel*io, void *userdata), void *userdata) {
    assert(io);
    io->callback = callback;
    io->userdata = userdata;
}

void pa_iochannel_set_noclose(struct pa_iochannel*io, int b) {
    assert(io);
    io->no_close = b;
}

void pa_iochannel_socket_peer_to_string(struct pa_iochannel*io, char*s, size_t l) {
    assert(io && s && l);
    pa_socket_peer_to_string(io->ifd, s, l);
}

int pa_iochannel_socket_set_rcvbuf(struct pa_iochannel *io, size_t l) {
    assert(io);
    return pa_socket_set_rcvbuf(io->ifd, l);
}

int pa_iochannel_socket_set_sndbuf(struct pa_iochannel *io, size_t l) {
    assert(io);
    return pa_socket_set_sndbuf(io->ofd, l);
}


struct pa_mainloop_api* pa_iochannel_get_mainloop_api(struct pa_iochannel *io) {
    assert(io);
    return io->mainloop;
}
