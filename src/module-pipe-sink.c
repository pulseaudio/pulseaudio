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
    char *filename;
    
    struct pa_sink *sink;
    struct pa_iochannel *io;
    struct pa_core *core;
    void *mainloop_source;
    struct pa_mainloop_api *mainloop;

    struct pa_memchunk memchunk;
};

static void do_write(struct userdata *u) {
    ssize_t r;
    assert(u);

    u->mainloop->enable_fixed(u->mainloop, u->mainloop_source, 0);
        
    if (!pa_iochannel_is_writable(u->io))
        return;

    if (!u->memchunk.length)
        if (pa_sink_render(u->sink, PIPE_BUF, &u->memchunk) < 0)
            return;

    assert(u->memchunk.memblock && u->memchunk.length);
    
    if ((r = pa_iochannel_write(u->io, u->memchunk.memblock->data + u->memchunk.index, u->memchunk.length)) < 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        return;
    }

    u->memchunk.index += r;
    u->memchunk.length -= r;
        
    if (u->memchunk.length <= 0) {
        pa_memblock_unref(u->memchunk.memblock);
        u->memchunk.memblock = NULL;
    }
}

static void notify_cb(struct pa_sink*s) {
    struct userdata *u = s->userdata;
    assert(s && u);

    if (pa_iochannel_is_writable(u->io))
        u->mainloop->enable_fixed(u->mainloop, u->mainloop_source, 1);
}

static void fixed_callback(struct pa_mainloop_api *m, void *id, void *userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
}

static void io_callback(struct pa_iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
}

int module_init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = NULL;
    struct stat st;
    char *p;
    int fd = -1;
    static const struct pa_sample_spec ss = {
        .format = PA_SAMPLE_S16NE,
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

    
    u = malloc(sizeof(struct userdata));
    assert(u);

    u->filename = strdup(p);
    assert(u->filename);
    u->core = c;
    u->sink = pa_sink_new(c, "fifo", 0, &ss);
    assert(u->sink);
    u->sink->notify = notify_cb;
    u->sink->userdata = u;

    u->io = pa_iochannel_new(c->mainloop, -1, fd);
    assert(u->io);
    pa_iochannel_set_callback(u->io, io_callback, u);

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;

    u->mainloop = c->mainloop;
    u->mainloop_source = u->mainloop->source_fixed(u->mainloop, fixed_callback, u);
    assert(u->mainloop_source);
    u->mainloop->enable_fixed(u->mainloop, u->mainloop_source, 0);
        
    m->userdata = u;

    return 0;

fail:
    if (fd >= 0)
        close(fd);

    return -1;
}

void module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    u = m->userdata;
    assert(u);
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
        
    pa_sink_free(u->sink);
    pa_iochannel_free(u->io);
    u->mainloop->cancel_fixed(u->mainloop, u->mainloop_source);

    assert(u->filename);
    unlink(u->filename);
    free(u->filename);
    
    free(u);
}
