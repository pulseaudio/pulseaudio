#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

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
#include "util.h"
#include "modargs.h"

#define DEFAULT_FIFO_NAME "/tmp/musicfifo"
#define DEFAULT_SINK_NAME "fifo_output"

struct userdata {
    struct pa_core *core;

    char *filename;
    
    struct pa_sink *sink;
    struct pa_iochannel *io;
    void *mainloop_source;

    struct pa_memchunk memchunk;
};

static const char* const valid_modargs[] = {
    "file",
    "rate",
    "channels",
    "format",
    "sink_name",
    NULL
};

static void do_write(struct userdata *u) {
    ssize_t r;
    assert(u);

    u->core->mainloop->enable_fixed(u->core->mainloop, u->mainloop_source, 0);
        
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
        u->core->mainloop->enable_fixed(u->core->mainloop, u->mainloop_source, 1);
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

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = NULL;
    struct stat st;
    const char *p;
    int fd = -1;
    struct pa_sample_spec ss;
    struct pa_modargs *ma = NULL;
    assert(c && m);
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, __FILE__": failed to parse module arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        fprintf(stderr, __FILE__": invalid sample format specification\n");
        goto fail;
    }
    
    mkfifo(p = pa_modargs_get_value(ma, "file", DEFAULT_FIFO_NAME), 0777);

    if ((fd = open(p, O_RDWR)) < 0) {
        fprintf(stderr, __FILE__": open('%s'): %s\n", p, strerror(errno));
        goto fail;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(stderr, __FILE__": fstat('%s'): %s\n", p, strerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        fprintf(stderr, __FILE__": '%s' is not a FIFO.\n", p);
        goto fail;
    }

    u = malloc(sizeof(struct userdata));
    assert(u);
    memset(u, 0, sizeof(struct userdata));

    u->filename = strdup(p);
    assert(u->filename);
    u->core = c;
    
    if (!(u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss))) {
        fprintf(stderr, __FILE__": failed to create sink.\n");
        goto fail;
    }
    u->sink->notify = notify_cb;
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("Unix FIFO sink '%s'", p);
    assert(u->sink->description);

    u->io = pa_iochannel_new(c->mainloop, -1, fd);
    assert(u->io);
    pa_iochannel_set_callback(u->io, io_callback, u);

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;

    u->mainloop_source = c->mainloop->source_fixed(c->mainloop, fixed_callback, u);
    assert(u->mainloop_source);
    c->mainloop->enable_fixed(c->mainloop, u->mainloop_source, 0);
        
    m->userdata = u;

    pa_modargs_free(ma);
    
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
        
    if (fd >= 0)
        close(fd);

    pa_module_done(c, m);

    return -1;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
        
    pa_sink_free(u->sink);
    pa_iochannel_free(u->io);
    u->core->mainloop->cancel_fixed(u->core->mainloop, u->mainloop_source);

    assert(u->filename);
    unlink(u->filename);
    free(u->filename);
    
    free(u);
}
