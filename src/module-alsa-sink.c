#include <assert.h>
#include <stdio.h>
#include <sys/poll.h>

#include <asoundlib.h>

#include "module.h"
#include "core.h"
#include "memchunk.h"
#include "sink.h"
#include "modargs.h"
#include "util.h"
#include "sample-util.h"

struct userdata {
    snd_pcm_t *pcm_handle;
    struct pa_sink *sink;
    void **io_sources;
    unsigned n_io_sources;

    size_t frame_size, fragment_size;
    struct pa_memchunk memchunk, silence;
};

static const char* const valid_modargs[] = {
    "device",
    "sink_name",
    NULL
};

#define DEFAULT_SINK_NAME "alsa_output"
#define DEFAULT_DEVICE "plughw:0,0"

static void xrun_recovery(struct userdata *u) {
    assert(u);

    fprintf(stderr, "*** XRUN ***\n");
    
    if (snd_pcm_prepare(u->pcm_handle) < 0)
        fprintf(stderr, "snd_pcm_prepare() failed\n");
}

static void do_write(struct userdata *u) {
    assert(u);

    for (;;) {
        struct pa_memchunk *memchunk = NULL;
        snd_pcm_sframes_t frames;
        
        if (u->memchunk.memblock)
            memchunk = &u->memchunk;
        else {
            if (pa_sink_render(u->sink, u->fragment_size, &u->memchunk) < 0)
                memchunk = &u->silence;
            else
                memchunk = &u->memchunk;
        }
            
        assert(memchunk->memblock && memchunk->memblock->data && memchunk->length && memchunk->memblock->length && (memchunk->length % u->frame_size) == 0);

        assert(u->pcm_handle);
        assert(memchunk->memblock);
        assert(memchunk->memblock->data);
        assert(memchunk->length);
        assert(u->frame_size);
        
        if ((frames = snd_pcm_writei(u->pcm_handle, memchunk->memblock->data + memchunk->index, memchunk->length / u->frame_size)) < 0) {
            if (frames == -EPIPE) {
                xrun_recovery(u);
                continue;
            }

            fprintf(stderr, "snd_pcm_writei() failed\n");
            return;
        }

        if (memchunk == &u->memchunk) {
            memchunk->index += frames * u->frame_size;
            memchunk->length -= frames * u->frame_size;

            if (memchunk->length == 0) {
                pa_memblock_unref(memchunk->memblock);
                memchunk->memblock = NULL;
                memchunk->index = memchunk->length = 0;
            }
        }
        
        break;
    }
}

static void io_callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    struct userdata *u = userdata;
    assert(u && a && id);

    if (snd_pcm_state(u->pcm_handle) == SND_PCM_STATE_XRUN)
        xrun_recovery(u);

    do_write(u);
}

static uint32_t sink_get_latency_cb(struct pa_sink *s) {
    struct userdata *u = s->userdata;
    snd_pcm_sframes_t frames;
    assert(s && u && u->sink);

    if (snd_pcm_delay(u->pcm_handle, &frames) < 0) {
        fprintf(stderr, __FILE__": failed to get delay\n");
        s->get_latency = NULL;
        return 0;
    }

    if (frames < 0)
        frames = 0;
    
    return pa_samples_usec(frames * u->frame_size, &s->sample_spec);
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;
    snd_pcm_hw_params_t *hwparams;
    const char *dev;
    struct pollfd *pfds, *ppfd;
    struct pa_sample_spec ss;
    unsigned i, periods;
    snd_pcm_uframes_t buffer_size;
    void ** ios;
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, __FILE__": failed to parse module arguments\n");
        goto fail;
    }
    
    u = malloc(sizeof(struct userdata));
    assert(u);
    memset(u, 0, sizeof(struct userdata));
    m->userdata = u;
    
    if (snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        fprintf(stderr, "Error opening PCM device %s\n", dev);
        goto fail;
    }
    
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 44100;
    ss.channels = 2;

    periods = 12;
    buffer_size = periods*256;

    snd_pcm_hw_params_alloca(&hwparams);
    if (snd_pcm_hw_params_any(u->pcm_handle, hwparams) < 0 ||
        snd_pcm_hw_params_set_access(u->pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
        snd_pcm_hw_params_set_format(u->pcm_handle, hwparams, SND_PCM_FORMAT_S16_LE) < 0 ||
        snd_pcm_hw_params_set_rate_near(u->pcm_handle, hwparams, &ss.rate, NULL) < 0 ||
        snd_pcm_hw_params_set_channels(u->pcm_handle, hwparams, ss.channels) < 0 ||
        snd_pcm_hw_params_set_periods_near(u->pcm_handle, hwparams, &periods, NULL) < 0 || 
        snd_pcm_hw_params_set_buffer_size_near(u->pcm_handle, hwparams, &buffer_size) < 0 || 
        snd_pcm_hw_params(u->pcm_handle, hwparams) < 0) {
        fprintf(stderr, "Error setting HW params.\n");
        goto fail;
    }

    u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss);
    assert(u->sink);

    u->sink->get_latency = sink_get_latency_cb;
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("Advanced Linux Sound Architecture PCM on '%s'", dev);

    u->n_io_sources = snd_pcm_poll_descriptors_count(u->pcm_handle);

    pfds = malloc(sizeof(struct pollfd) * u->n_io_sources);
    assert(pfds);
    if (snd_pcm_poll_descriptors(u->pcm_handle, pfds, u->n_io_sources) < 0) {
        printf("Unable to obtain poll descriptors for playback.\n");
        free(pfds);
        goto fail;
    }
    
    u->io_sources = malloc(sizeof(void*) * u->n_io_sources);
    assert(u->io_sources);

    for (i = 0, ios = u->io_sources, ppfd = pfds; i < u->n_io_sources; i++, ios++, ppfd++) {
        *ios = c->mainloop->source_io(c->mainloop, ppfd->fd,
                                      ((ppfd->events & POLLIN) ? PA_MAINLOOP_API_IO_EVENT_INPUT : 0) |
                                      ((ppfd->events & POLLOUT) ? PA_MAINLOOP_API_IO_EVENT_OUTPUT : 0), io_callback, u);
        assert(*ios);
    }

    free(pfds);

    u->frame_size = pa_sample_size(&ss);
    u->fragment_size = buffer_size*u->frame_size/periods;

    fprintf(stderr, __FILE__": using %u fragments of size %u bytes.\n", periods, u->fragment_size);

    u->silence.memblock = pa_memblock_new(u->silence.length = u->fragment_size);
    assert(u->silence.memblock);
    pa_silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;

    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;
    
    ret = 0;
     
finish:
     if (ma)
         pa_modargs_free(ma);
    
    return ret;

fail:
    
    if (u)
        pa_module_done(c, m);

    goto finish;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if ((u = m->userdata)) {
        unsigned i;
        void **ios;
        
        pa_sink_free(u->sink);

        for (ios = u->io_sources, i = 0; i < u->n_io_sources; i++, ios++)
            c->mainloop->cancel_io(c->mainloop, *ios);
        free(u->io_sources);
        
        if (u->pcm_handle) {
            snd_pcm_drop(u->pcm_handle);
            snd_pcm_close(u->pcm_handle);
        }

        if (u->memchunk.memblock)
            pa_memblock_unref(u->memchunk.memblock);
        if (u->silence.memblock)
            pa_memblock_unref(u->silence.memblock);
        
        free(u);
    }
}

