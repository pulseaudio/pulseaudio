#include <soundcard.h>
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
#include "source.h"
#include "module.h"

struct userdata {
    struct sink *sink;
    struct source *source;
    struct iochannel *io;
    struct core *core;

    struct memchunk memchunk, silence;

    uint32_t fragment_size;
};

static void do_write(struct userdata *u) {
    struct memchunk *memchunk;
    ssize_t r;
    assert(u);

    if (!iochannel_is_writable(u->io))
        return;

    if (!u->memchunk.length) {
        if (sink_render(u->sink, fragment_size, &u->memchunk) < 0)
            memchunk = &u->silence;
        else
            memchunk = &u->memchunk;
    }

    assert(memchunk->memblock && memchunk->length);
    
    if ((r = iochannel_write(u->io, memchunk->memblock->data + memchunk->index, memchunk->length)) < 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        return;
    }

    if (memchunk == &u->silence)
        assert(r % u->sample_size == 0);
    else {
        u->memchunk.index += r;
        u->memchunk.length -= r;
        
        if (u->memchunk.length <= 0) {
            memblock_unref(u->memchunk.memblock);
            u->memchunk.memblock = NULL;
        }
    }
}

static void do_read(struct userdata *u) {
    struct memchunk memchunk;
    ssize_t r;
    assert(u);
    
    if (!iochannel_is_readable(u->io))
        return;

    memchunk.memblock = memblock_new(u->fragment_size);
    assert(memchunk.memblock);
    if ((r = iochannel_read(u->io, memchunk.memblock->data, memchunk->memblock->length)) < 0) {
        memblock_unref(memchunk.memblock);
        fprintf(stderr, "read() failed: %s\n", strerror(errno));
        return;
    }

    assert(r < memchunk->memblock->length);
    memchunk.length = memchunk.memblock->length = r;
    memchunk.index = 0;

    source_post(u->source, &memchunk);
    memblock_unref(memchunk.memblock);
};

static void io_callback(struct iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
    do_read(u);
}

int module_init(struct core *c, struct module*m) {
    struct userdata *u = NULL;
    struct stat st;
    char *p;
    int fd = -1;
    int format, channels, speed, frag_size;
    int m;
    const static struct sample_spec ss;
    assert(c && m);

    p = m->argument ? m->argument : "/dev/dsp";
    if ((fd = open(p, m = O_RDWR)) >= 0) {
        int caps;

        ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0);
        
        if (ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
            goto fail;
        }

        if (!(caps & DSP_CAP_DUPLEX)) {
            close(fd);
            fd = -1;
        }
    }

    if (fd < 0) {
        if ((fd = open(p, m = O_WRONLY)) < 0) {
            if ((fd = open(p, m = O_RDONLY)) < 0) {
                fprintf(stderr, "open('%s'): %s\n", p, strerror(errno));
                goto fail;
            }
        }
    }

    frags = 0x7fff0000 | (10 << 16); /* 1024 bytes fragment size */
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frags) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SETFRAGMENT: %s\n", strerror(errno));
        goto fail;
    }

    format = AFMT_S16_NE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_S16_NE) {
        int f = AFMT_S16_NE == AFMT_S16_LE ? AFMT_S16_BE : AFMT_S16_LE;
        format = f;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != f) {
            format = AFMT_U8;
            if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_U8) {
                fprintf(stderr, "SNDCTL_DSP_SETFMT: %s\n", format != AFMT_U8 ? "No supported sample format" : strerror(errno));
                goto fail;
            } else
                ss.format = SAMPLE_U8;
        } else
            ss.format = f == AFMT_S16_LE ? SAMPLE_S16LE : SAMPLE_S16BE;
    } else
        ss.format = SAMPLE_S16NE;
        
    channels = 2;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
        fprintf(stderr, "SNDCTL_DSP_CHANNELS: %s\n", strerror(errno));
        goto fail;
    }
    assert(channels);
    ss.channels = channels;

    speed = 44100;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &speed) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SPEED: %s\n", strerror(errno));
        goto fail;
    }
    assert(speed);
    ss.rate = speed;

    if (ioctl(fd, SNCTL_DSP_GETBLKSIZE, &frag_size) < 0) {
        fprintf(stderr, "SNDCTL_DSP_GETBLKSIZE: %s\n", strerror(errno));
        goto fail;
    }

    assert(frag_size);
    
    u = malloc(sizeof(struct userdata));
    assert(u);

    u->core = c;

    if (m != O_RDONLY) {
        u->sink = sink_new(c, "dsp", &ss);
        assert(u->sink);
    } else
        u->sink = NULL;

    if (m != O_WRONLY) {
        u->source = source_new(c, "dsp", &ss);
        assert(u->source);
    } else
        u->source = NULL;

    assert(u->source || u->sink);

    u->io = iochannel_new(c->mainloop, u->source ? fd : -1, u->sink ? fd : 0);
    assert(u->io);
    iochannel_set_callback(u->io, io_callback, u);

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;

    u->fragment_size = frag_size;
    u->silence.memblock = memblock_new(u->fragment_size);
    assert(u->silence);
    u->silence.length = u->fragment_size;
    u->silence.index = 0;
    
    m->userdata = u;

    return 0;

fail:
    if (fd >= 0)
        close(fd);

    return -1;
}

void module_done(struct core *c, struct module*m) {
    struct userdata *u;
    assert(c && m);

    u = m->userdata;
    assert(u);
    
    if (u->memchunk.memblock)
        memblock_unref(u->memchunk.memblock);
    if (u->silence.memblock)
        memblock_unref(u->silence.memblock);
        
    sink_free(u->sink);
    source_free(u->source);
    iochannel_free(u->io);
    free(u);
}
