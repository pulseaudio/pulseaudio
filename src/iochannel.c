#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "iochannel.h"
#include "util.h"

struct iochannel {
    int ifd, ofd;
    struct pa_mainloop_api* mainloop;

    void (*callback)(struct iochannel*io, void *userdata);
    void*userdata;
    
    int readable;
    int writable;
    
    int no_close;

    void* input_source, *output_source;
};

static void enable_mainloop_sources(struct iochannel *io) {
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
    struct iochannel *io = userdata;
    int changed = 0;
    assert(m && fd >= 0 && events && userdata);

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

struct iochannel* iochannel_new(struct pa_mainloop_api*m, int ifd, int ofd) {
    struct iochannel *io;
    assert(m && (ifd >= 0 || ofd >= 0));

    io = malloc(sizeof(struct iochannel));
    io->ifd = ifd;
    io->ofd = ofd;
    io->mainloop = m;

    io->userdata = NULL;
    io->callback = NULL;
    io->readable = 0;
    io->writable = 0;
    io->no_close = 0;

    if (ifd == ofd) {
        assert(ifd >= 0);
        make_nonblock_fd(io->ifd);
        io->input_source = io->output_source = m->source_io(m, ifd, PA_MAINLOOP_API_IO_EVENT_BOTH, callback, io);
    } else {

        if (ifd >= 0) {
            make_nonblock_fd(io->ifd);
            io->input_source = m->source_io(m, ifd, PA_MAINLOOP_API_IO_EVENT_INPUT, callback, io);
        } else
            io->input_source = NULL;

        if (ofd >= 0) {
            make_nonblock_fd(io->ofd);
            io->output_source = m->source_io(m, ofd, PA_MAINLOOP_API_IO_EVENT_OUTPUT, callback, io);
        } else
            io->output_source = NULL;
    }

    return io;
}

void iochannel_free(struct iochannel*io) {
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
    
    free(io);
}

int iochannel_is_readable(struct iochannel*io) {
    assert(io);
    return io->readable;
}

int iochannel_is_writable(struct iochannel*io) {
    assert(io);
    return io->writable;
}

ssize_t iochannel_write(struct iochannel*io, const void*data, size_t l) {
    ssize_t r;
    assert(io && data && l && io->ofd >= 0);

    if ((r = write(io->ofd, data, l)) >= 0) {
        io->writable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

ssize_t iochannel_read(struct iochannel*io, void*data, size_t l) {
    ssize_t r;
    
    assert(io && data && io->ifd >= 0);
    
    if ((r = read(io->ifd, data, l)) >= 0) {
        io->readable = 0;
        enable_mainloop_sources(io);
    }

    return r;
}

void iochannel_set_callback(struct iochannel*io, void (*callback)(struct iochannel*io, void *userdata), void *userdata) {
    assert(io);
    io->callback = callback;
    io->userdata = userdata;
}

void iochannel_set_noclose(struct iochannel*io, int b) {
    assert(io);
    io->no_close = b;
}

void iochannel_peer_to_string(struct iochannel*io, char*s, size_t l) {
    assert(io && s && l);
    peer_to_string(s, l, io->ifd);
}
