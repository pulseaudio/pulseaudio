/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
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

    void* input_source, *output_source;
};

static void enable_mainloop_sources(struct pa_iochannel *io) {
    assert(io);

    if (io->input_source == io->output_source) {
        enum pa_mainloop_api_io_events e = PA_MAINLOOP_API_IO_EVENT_NULL;
        assert(io->input_source);
        
        if (!io->readable)
            e |= PA_MAINLOOP_API_IO_EVENT_INPUT;
        if (!io->writable)
            e |= PA_MAINLOOP_API_IO_EVENT_OUTPUT;

        io->mainloop->enable_io(io->mainloop, io->input_source, e);
    } else {
        if (io->input_source)
            io->mainloop->enable_io(io->mainloop, io->input_source, io->readable ? PA_MAINLOOP_API_IO_EVENT_NULL : PA_MAINLOOP_API_IO_EVENT_INPUT);
        if (io->output_source)
            io->mainloop->enable_io(io->mainloop, io->output_source, io->writable ? PA_MAINLOOP_API_IO_EVENT_NULL : PA_MAINLOOP_API_IO_EVENT_OUTPUT);
    }
}

static void callback(struct pa_mainloop_api* m, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    struct pa_iochannel *io = userdata;
    int changed = 0;
    assert(m && fd >= 0 && events && userdata);

    if ((events & PA_MAINLOOP_API_IO_EVENT_HUP) && !io->hungup) {
        io->hungup = 1;
        changed = 1;
    }
    
    if ((events & PA_MAINLOOP_API_IO_EVENT_INPUT) && !io->readable) {
        io->readable = 1;
        changed = 1;
        assert(id == io->input_source);
    }
    
    if ((events & PA_MAINLOOP_API_IO_EVENT_OUTPUT) && !io->writable) {
        io->writable = 1;
        changed = 1;
        assert(id == io->output_source);
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

    if (ifd == ofd) {
        assert(ifd >= 0);
        pa_make_nonblock_fd(io->ifd);
        io->input_source = io->output_source = m->source_io(m, ifd, PA_MAINLOOP_API_IO_EVENT_BOTH, callback, io);
    } else {

        if (ifd >= 0) {
            pa_make_nonblock_fd(io->ifd);
            io->input_source = m->source_io(m, ifd, PA_MAINLOOP_API_IO_EVENT_INPUT, callback, io);
        } else
            io->input_source = NULL;

        if (ofd >= 0) {
            pa_make_nonblock_fd(io->ofd);
            io->output_source = m->source_io(m, ofd, PA_MAINLOOP_API_IO_EVENT_OUTPUT, callback, io);
        } else
            io->output_source = NULL;
    }

    return io;
}

void pa_iochannel_free(struct pa_iochannel*io) {
    assert(io);

    if (!io->no_close) {
        if (io->ifd >= 0)
            close(io->ifd);
        if (io->ofd >= 0 && io->ofd != io->ifd)
            close(io->ofd);
    }

    if (io->input_source)
        io->mainloop->cancel_io(io->mainloop, io->input_source);
    if (io->output_source && (io->output_source != io->input_source))
        io->mainloop->cancel_io(io->mainloop, io->output_source);
    
    pa_xfree(io);
}

int pa_iochannel_is_readable(struct pa_iochannel*io) {
    assert(io);
    return io->readable;
}

int pa_iochannel_is_writable(struct pa_iochannel*io) {
    assert(io);
    return io->writable;
}

int pa_iochannel_is_hungup(struct pa_iochannel*io) {
    assert(io);
    return io->hungup;
}

ssize_t pa_iochannel_write(struct pa_iochannel*io, const void*data, size_t l) {
    ssize_t r;
    assert(io && data && l && io->ofd >= 0);

    if ((r = write(io->ofd, data, l)) >= 0) {
        io->writable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

ssize_t pa_iochannel_read(struct pa_iochannel*io, void*data, size_t l) {
    ssize_t r;
    
    assert(io && data && io->ifd >= 0);
    
    if ((r = read(io->ifd, data, l)) >= 0) {
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
