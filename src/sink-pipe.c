#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "iochannel.h"
#include "sink.h"
#include "module.h"

struct userdata {
    struct sink *sink;
    struct iochannel *io;
    struct core *core;
    struct mainloop_source *mainloop_source;

    struct memchunk memchunk;
};

static void do_write(struct userdata *u) {
    ssize_t r;
    assert(u);

    mainloop_source_enable(u->mainloop_source, 0);
        
    if (!iochannel_is_writable(u->io))
        return;

    if (!u->memchunk.length)
        if (sink_render(u->sink, PIPE_BUF, &u->memchunk) < 0)
            return;

    assert(u->memchunk.memblock && u->memchunk.length);
    
    if ((r = iochannel_write(u->io, u->memchunk.memblock->data + u->memchunk.index, u->memchunk.length)) < 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        return;
    }

    u->memchunk.index += r;
    u->memchunk.length -= r;
        
    if (u->memchunk.length <= 0) {
        memblock_unref(u->memchunk.memblock);
        u->memchunk.memblock = NULL;
    }
}

static void notify_callback(struct sink*s, void *userdata) {
    struct userdata *u = userdata;
    assert(u);

    if (iochannel_is_writable(u->io))
        mainloop_source_enable(u->mainloop_source, 1);
}

static void prepare_callback(struct mainloop_source *src, void *userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
}

static void io_callback(struct iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
}

int module_init(struct core *c, struct module*m) {
    struct userdata *u = NULL;
    struct stat st;
    struct sink *sink;
    char *p;
    int fd = -1;
    const static struct sample_spec ss = {
        .format = SAMPLE_S16NE,
        .rate = 44100,
        .channels = 2,
    };
    assert(c && m);

    mkfifo((p = m->argument ? m->argument : "/tmp/musicfifo"), 0777);

    if ((fd = open(p, O_RDWR)) < 0) {
        fprintf(stderr, "open('%s'): %s\n", p, strerror(errno));
        goto fail;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "fstat('%s'): %s\n", p, strerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "'%s' is not a FIFO\n", p);
        goto fail;
    }

    if (!(sink = sink_new(c, "fifo", &ss))) {
        fprintf(stderr, "Failed to allocate new sink!\n");
        goto fail;
    }
    
    u = malloc(sizeof(struct userdata));
    assert(u);

    u->core = c;
    u->sink = sink;
    sink_set_notify_callback(sink, notify_callback, u);

    u->io = iochannel_new(c->mainloop, -1, fd);
    assert(u->io);
    iochannel_set_callback(u->io, io_callback, u);

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;

    u->mainloop_source = mainloop_source_new_prepare(c->mainloop, prepare_callback, u);
    assert(u->mainloop_source);
    mainloop_source_enable(u->mainloop_source, 0);
    
    m->userdata = u;


    return 0;

fail:
    if (fd >= 0)
        close(fd);

    if (u)
        free(u);
    
    return -1;
}

void module_done(struct core *c, struct module*m) {
    struct userdata *u;
    assert(c && m);

    u = m->userdata;
    assert(u);
    
    if (u->memchunk.memblock)
        memblock_unref(u->memchunk.memblock);
        
    sink_free(u->sink);
    iochannel_free(u->io);
    mainloop_source_free(u->mainloop_source);
    free(u);
}
