#include <asoundlib.h>

#include "alsa-util.h"
#include "sample.h"

int pa_alsa_set_hw_params(snd_pcm_t *pcm_handle, struct pa_sample_spec *ss, uint32_t *periods, snd_pcm_uframes_t *buffer_size) {
    int ret = 0;
    snd_pcm_hw_params_t *hwparams = NULL;
    static const snd_pcm_format_t format_trans[] = {
        [PA_SAMPLE_U8] = SND_PCM_FORMAT_U8,
        [PA_SAMPLE_ALAW] = SND_PCM_FORMAT_A_LAW,
        [PA_SAMPLE_ULAW] = SND_PCM_FORMAT_MU_LAW,
        [PA_SAMPLE_S16LE] = SND_PCM_FORMAT_S16_LE,
        [PA_SAMPLE_S16BE] = SND_PCM_FORMAT_S16_BE,
        [PA_SAMPLE_FLOAT32LE] = SND_PCM_FORMAT_FLOAT_LE,
        [PA_SAMPLE_FLOAT32BE] = SND_PCM_FORMAT_FLOAT_BE,
    };

    if (snd_pcm_hw_params_malloc(&hwparams) < 0 ||
        snd_pcm_hw_params_any(pcm_handle, hwparams) < 0 ||
        snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
        snd_pcm_hw_params_set_format(pcm_handle, hwparams, format_trans[ss->format]) < 0 ||
        snd_pcm_hw_params_set_rate_near(pcm_handle, hwparams, &ss->rate, NULL) < 0 ||
        snd_pcm_hw_params_set_channels(pcm_handle, hwparams, ss->channels) < 0 ||
        snd_pcm_hw_params_set_periods_near(pcm_handle, hwparams, periods, NULL) < 0 || 
        snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hwparams, buffer_size) < 0 || 
        snd_pcm_hw_params(pcm_handle, hwparams) < 0) {
        ret = -1;
    }

    if (hwparams)
        snd_pcm_hw_params_free(hwparams);
    return ret;
}

int pa_create_io_sources(snd_pcm_t *pcm_handle, struct pa_mainloop_api* m, void ***io_sources, unsigned *n_io_sources, void (*cb)(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata), void *userdata) {
    unsigned i;
    struct pollfd *pfds, *ppfd;
    void **ios;
    assert(pcm_handle && m && io_sources && n_io_sources && cb);

    *n_io_sources = snd_pcm_poll_descriptors_count(pcm_handle);

    pfds = malloc(sizeof(struct pollfd) * *n_io_sources);
    assert(pfds);
    if (snd_pcm_poll_descriptors(pcm_handle, pfds, *n_io_sources) < 0) {
        free(pfds);
        return -1;
    }
    
    *io_sources = malloc(sizeof(void*) * *n_io_sources);
    assert(io_sources);

    for (i = 0, ios = *io_sources, ppfd = pfds; i < *n_io_sources; i++, ios++, ppfd++) {
        *ios = m->source_io(m, ppfd->fd,
                            ((ppfd->events & POLLIN) ? PA_MAINLOOP_API_IO_EVENT_INPUT : 0) |
                            ((ppfd->events & POLLOUT) ? PA_MAINLOOP_API_IO_EVENT_OUTPUT : 0), cb, userdata);
        assert(*ios);
    }

    free(pfds);
    return 0;
}

void pa_free_io_sources(struct pa_mainloop_api* m, void **io_sources, unsigned n_io_sources) {
    unsigned i;
    void **ios;
    assert(m && io_sources);
    
    for (ios = io_sources, i = 0; i < n_io_sources; i++, ios++)
        m->cancel_io(m, *ios);
    free(io_sources);
}
