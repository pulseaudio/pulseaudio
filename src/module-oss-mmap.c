#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>

#include "iochannel.h"
#include "sink.h"
#include "source.h"
#include "module.h"
#include "oss.h"

struct userdata {
    struct sink *sink;
    struct source *source;
    struct core *core;
    struct sample_spec sample_spec;

    size_t in_fragment_size, out_fragment_size, in_fragments, out_fragments, sample_size, out_fill;
    uint32_t sample_usec;

    int fd;

    void *in_mmap, *out_mmap;
    size_t in_mmap_length, out_mmap_length;

    struct mainloop_source *mainloop_source;

    struct memblock **in_memblocks, **out_memblocks;
    unsigned out_current, in_current;
};

void module_done(struct core *c, struct module*m);

static void out_clear_memblocks(struct userdata*u, unsigned n) {
    unsigned i = u->out_current;
    assert(u && u->out_memblocks);

    if (n > u->out_fragments)
        n = u->out_fragments;
    
    while (n > 0) {
        if (u->out_memblocks[i]) {
            memblock_unref_fixed(u->out_memblocks[i]);
            u->out_memblocks[i] = NULL;
        }

        i++;
        while (i >= u->out_fragments)
            i -= u->out_fragments;

        n--;
    }
}

static void out_fill_memblocks(struct userdata *u, unsigned n) {
    assert(u && u->out_memblocks);
    
    while (n > 0) {
        struct memchunk chunk;
        
        if (!u->out_memblocks[u->out_current]) {

            u->out_memblocks[u->out_current] = chunk.memblock = memblock_new_fixed(u->out_mmap+u->out_fragment_size*u->out_current,  u->out_fragment_size);
            chunk.length = chunk.memblock->length;
            chunk.index = 0;
            
            sink_render_into_full(u->sink, &chunk);
        }
            
        u->out_current++;
        while (u->out_current >= u->out_fragments)
            u->out_current -= u->out_fragments;
        
        n--;
    }
}

static void do_write(struct userdata *u) {
    struct count_info info;
    assert(u && u->sink);

    if (ioctl(u->fd, SNDCTL_DSP_GETOPTR, &info) < 0) {
        fprintf(stderr, "SNDCTL_DSP_GETOPTR: %s\n", strerror(errno));
        return;
    }

    u->out_fill = (u->out_fragment_size * u->out_fragments) - (info.ptr % u->out_fragment_size);

    if (!info.blocks)
        return;
    
    out_clear_memblocks(u, info.blocks);
    out_fill_memblocks(u, info.blocks);

}

static void in_post_memblocks(struct userdata *u, unsigned n) {
    assert(u && u->in_memblocks);
    
    while (n > 0) {
        struct memchunk chunk;
        
        if (!u->in_memblocks[u->in_current]) {
            u->in_memblocks[u->in_current] = chunk.memblock = memblock_new_fixed(u->in_mmap+u->in_fragment_size*u->in_current, u->in_fragment_size);
            chunk.length = chunk.memblock->length;
            chunk.index = 0;
            
            source_post(u->source, &chunk);
        }

        u->in_current++;
        while (u->in_current >= u->in_fragments)
            u->in_current -= u->in_fragments;
        
        n--;
    }
}

static void in_clear_memblocks(struct userdata*u, unsigned n) {
    unsigned i = u->in_current;
    assert(u && u->in_memblocks);

    if (n > u->in_fragments)
        n = u->in_fragments;
    
    while (n > 0) {
        if (u->in_memblocks[i]) {
            memblock_unref_fixed(u->in_memblocks[i]);
            u->in_memblocks[i] = NULL;
        }

        i++;
        while (i >= u->in_fragments)
            i -= u->in_fragments;

        n--;
    }
}

static void do_read(struct userdata *u) {
    struct count_info info;
    assert(u && u->source);

    if (ioctl(u->fd, SNDCTL_DSP_GETIPTR, &info) < 0) {
        fprintf(stderr, "SNDCTL_DSP_GETIPTR: %s\n", strerror(errno));
        return;
    }

    if (!info.blocks)
        return;
    
    in_post_memblocks(u, info.blocks);
    in_clear_memblocks(u, u->in_fragments/2);
};

static void io_callback(struct mainloop_source*s, int fd, enum mainloop_io_event event, void *userdata) {
    struct userdata *u = userdata;

    if (event & MAINLOOP_IO_EVENT_IN)
        do_read(u);
    if (event & MAINLOOP_IO_EVENT_OUT)
        do_write(u);
}

static uint32_t sink_get_latency_cb(struct sink *s) {
    struct userdata *u = s->userdata;
    assert(s && u);

    do_write(u);
    return u->out_fill/u->sample_size*u->sample_usec;
}

int module_init(struct core *c, struct module*m) {
    struct audio_buf_info info;
    struct userdata *u = NULL;
    char *p;
    int frag_size;
    int mode, caps, caps_read = 0;
    int enable_bits = 0, zero = 0;
    assert(c && m);

    m->userdata = u = malloc(sizeof(struct userdata));
    assert(u);
    memset(u, 0, sizeof(struct userdata));
    u->fd = -1;
    u->core = c;
    
    p = m->argument ? m->argument : "/dev/dsp";
    if ((u->fd = open(p, (mode = O_RDWR)|O_NDELAY)) >= 0) {
        ioctl(u->fd, SNDCTL_DSP_SETDUPLEX, 0);
        
        if (ioctl(u->fd, SNDCTL_DSP_GETCAPS, &caps) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
            goto fail;
        }

        if (!(caps & DSP_CAP_DUPLEX)) {
            close(u->fd);
            u->fd = -1;
        } else
            caps_read = 1;
    }

    if (u->fd < 0) {
        if ((u->fd = open(p, (mode = O_WRONLY)|O_NDELAY)) < 0) {
            if ((u->fd = open(p, (mode = O_RDONLY)|O_NDELAY)) < 0) {
                fprintf(stderr, "open('%s'): %s\n", p, strerror(errno));
                goto fail;
            }
        }
    }

    if (!caps_read) {
        if (ioctl(u->fd, SNDCTL_DSP_GETCAPS, &caps) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
            goto fail;
        }
    }
        
    if (!(caps & DSP_CAP_MMAP) || !(caps & DSP_CAP_REALTIME) || !(caps & DSP_CAP_TRIGGER)) {
        fprintf(stderr, "OSS device not mmap capable.\n");
        goto fail;
    }

    fprintf(stderr, "module-oss: device opened in %s mode.\n", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));
    
    frag_size = ((int) 12 << 16) | 10; /* nfrags = 12; frag_size = 2^10 */
    if (ioctl(u->fd, SNDCTL_DSP_SETFRAGMENT, &frag_size) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SETFRAGMENT: %s\n", strerror(errno));
        goto fail;
    }

    if (oss_auto_format(u->fd, &u->sample_spec) < 0)
        goto fail;

    if (mode != O_WRONLY) {
        if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETISPACE: %s\n", strerror(errno));
            goto fail;
        }

        fprintf(stderr, "module-oss-mmap: input -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        u->in_mmap_length = (u->in_fragment_size = info.fragsize) * (u->in_fragments = info.fragstotal);

        if ((u->in_mmap = mmap(NULL, u->in_mmap_length, PROT_READ, MAP_SHARED, u->fd, 0)) == MAP_FAILED) {
            if (mode == O_RDWR) {
                fprintf(stderr, "module-oss-mmap: mmap failed for input. Changing to O_WRONLY mode.\n");
                mode = O_WRONLY;
            } else {
                fprintf(stderr, "modeule-oss-mmap: mmap(): %s\n", strerror(errno));
                goto fail;
            }
        } else {
        
            u->source = source_new(c, "dsp", &u->sample_spec);
            assert(u->source);
            u->source->userdata = u;
            
            u->in_memblocks = malloc(sizeof(struct memblock *)*u->in_fragments);
            memset(u->in_memblocks, 0, sizeof(struct memblock *)*u->in_fragments);
            
            enable_bits |= PCM_ENABLE_INPUT;
        }
    }

    if (m != O_RDONLY) {
        if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETOSPACE: %s\n", strerror(errno));
            goto fail;
        }
        
        fprintf(stderr, "module-oss: output -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        u->out_mmap_length = (u->out_fragment_size = info.fragsize) * (u->out_fragments = info.fragstotal);

        if ((u->out_mmap = mmap(NULL, u->out_mmap_length, PROT_WRITE, MAP_SHARED, u->fd, 0))  == MAP_FAILED) {
            if (mode == O_RDWR) {
                fprintf(stderr, "module-oss-mmap: mmap filed for output. Changing to O_RDONLY mode.\n");
                mode = O_RDONLY;
            } else {
                fprintf(stderr, "modeule-oss-mmap: mmap(): %s\n", strerror(errno));
                goto fail;
            }
        } else {
            silence_memory(u->out_mmap, u->out_mmap_length, &u->sample_spec);
            
            u->sink = sink_new(c, "dsp", &u->sample_spec);
            assert(u->sink);
            u->sink->get_latency = sink_get_latency_cb;
            u->sink->userdata = u;
            
            u->out_memblocks = malloc(sizeof(struct memblock *)*u->out_fragments);
            memset(u->out_memblocks, 0, sizeof(struct memblock *)*u->out_fragments);
            
            enable_bits |= PCM_ENABLE_OUTPUT;
        }
    }

    zero = 0;
    if (ioctl(u->fd, SNDCTL_DSP_SETTRIGGER, &zero) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SETTRIGGER: %s\n", strerror(errno));
        goto fail;
    }
    
    if (ioctl(u->fd, SNDCTL_DSP_SETTRIGGER, &enable_bits) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SETTRIGGER: %s\n", strerror(errno));
        goto fail;
    }
        
    assert(u->source || u->sink);

    u->sample_size = sample_size(&u->sample_spec);
    u->sample_usec = 1000000/u->sample_spec.rate;

    u->mainloop_source = mainloop_source_new_io(c->mainloop, u->fd, (u->source ? MAINLOOP_IO_EVENT_IN : 0) | (u->sink ? MAINLOOP_IO_EVENT_OUT : 0), io_callback, u);
    assert(u->mainloop_source);

    return 0;

fail:
    module_done(c, m);

    return -1;
}

void module_done(struct core *c, struct module*m) {
    struct userdata *u;
    assert(c && m);

    u = m->userdata;
    assert(u);

    if (u->out_memblocks) {
        out_clear_memblocks(u, u->out_fragments);
        free(u->out_memblocks);
    }

    if (u->in_memblocks) {
        in_clear_memblocks(u, u->in_fragments);
        free(u->in_memblocks);
    }
    
    if (u->in_mmap && u->in_mmap != MAP_FAILED)
        munmap(u->in_mmap, u->in_mmap_length);
    
    if (u->out_mmap && u->out_mmap != MAP_FAILED)
        munmap(u->out_mmap, u->out_mmap_length);
    
    if (u->sink)
        sink_free(u->sink);

    if (u->source)
        source_free(u->source);

    if (u->mainloop_source)
        mainloop_source_free(u->mainloop_source);

    if (u->fd >= 0)
        close(u->fd);

    free(u);
}
