#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "iochannel.h"

struct iochannel {
    int ifd, ofd;
    struct mainloop* mainloop;

    void (*callback)(struct iochannel*io, void *userdata);
    void*userdata;
    
    int readable;
    int writable;

    struct mainloop_source* input_source, *output_source;
};

static void enable_mainloop_sources(struct iochannel *io) {
    assert(io);

    if (io->input_source == io->output_source) {
        enum mainloop_io_event e = MAINLOOP_IO_EVENT_NULL;
        assert(io->input_source);
        
        if (!io->readable)
            e |= MAINLOOP_IO_EVENT_IN;
        if (!io->writable)
            e |= MAINLOOP_IO_EVENT_OUT;

        mainloop_source_io_set_events(io->input_source, e);
    } else {
        if (io->input_source)
            mainloop_source_io_set_events(io->input_source, io->readable ? MAINLOOP_IO_EVENT_NULL : MAINLOOP_IO_EVENT_IN);
        if (io->output_source)
            mainloop_source_io_set_events(io->output_source, io->writable ? MAINLOOP_IO_EVENT_NULL : MAINLOOP_IO_EVENT_OUT);
    }
}

static void callback(struct mainloop_source*s, int fd, enum mainloop_io_event events, void *userdata) {
    struct iochannel *io = userdata;
    int changed;
    assert(s && fd >= 0 && userdata);

    if (events & MAINLOOP_IO_EVENT_IN && !io->readable) {
        io->readable = 1;
        changed = 1;
    }
    
    if (events & MAINLOOP_IO_EVENT_OUT && !io->writable) {
        io->writable = 1;
        changed = 1;
    }

    if (changed) {
        enable_mainloop_sources(io);
        
        if (io->callback)
            io->callback(io, io->userdata);
    }
}

static void make_nonblock_fd(int fd) {
    int v;

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
}

struct iochannel* iochannel_new(struct mainloop*m, int ifd, int ofd) {
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

    if (ifd == ofd) {
        assert(ifd >= 0);
        make_nonblock_fd(io->ifd);
        io->input_source = io->output_source = mainloop_source_new_io(m, ifd, MAINLOOP_IO_EVENT_IN|MAINLOOP_IO_EVENT_OUT, callback, io);
    } else {

        if (ifd >= 0) {
            make_nonblock_fd(io->ifd);
            io->input_source = mainloop_source_new_io(m, ifd, MAINLOOP_IO_EVENT_IN, callback, io);
        } else
            io->input_source = NULL;

        if (ofd >= 0) {
            make_nonblock_fd(io->ofd);
            io->output_source = mainloop_source_new_io(m, ofd, MAINLOOP_IO_EVENT_OUT, callback, io);
        } else
            io->output_source = NULL;
    }

    return io;
}

void iochannel_free(struct iochannel*io) {
    assert(io);

    if (io->ifd >= 0)
        close(io->ifd);
    if (io->ofd >= 0 && io->ofd != io->ifd)
        close(io->ofd);

    if (io->input_source)
        mainloop_source_free(io->input_source);
    if (io->output_source)
        mainloop_source_free(io->output_source);
    
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
    
    assert(io && data && l && io->ifd >= 0);

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
