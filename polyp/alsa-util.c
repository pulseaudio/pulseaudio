/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <asoundlib.h>

#include "alsa-util.h"
#include "sample.h"
#include "xmalloc.h"

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

int pa_create_io_events(snd_pcm_t *pcm_handle, struct pa_mainloop_api* m, struct pa_io_event ***io_events, unsigned *n_io_events, void (*cb)(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags events, void *userdata), void *userdata) {
    unsigned i;
    struct pollfd *pfds, *ppfd;
    struct pa_io_event **ios;
    assert(pcm_handle && m && io_events && n_io_events && cb);

    *n_io_events = snd_pcm_poll_descriptors_count(pcm_handle);

    pfds = pa_xmalloc(sizeof(struct pollfd) * *n_io_events);
    if (snd_pcm_poll_descriptors(pcm_handle, pfds, *n_io_events) < 0) {
        pa_xfree(pfds);
        return -1;
    }
    
    *io_events = pa_xmalloc(sizeof(void*) * *n_io_events);

    for (i = 0, ios = *io_events, ppfd = pfds; i < *n_io_events; i++, ios++, ppfd++) {
        *ios = m->io_new(m, ppfd->fd,
                            ((ppfd->events & POLLIN) ? PA_IO_EVENT_INPUT : 0) |
                            ((ppfd->events & POLLOUT) ? PA_IO_EVENT_OUTPUT : 0), cb, userdata);
        assert(*ios);
    }

    pa_xfree(pfds);
    return 0;
}

void pa_free_io_events(struct pa_mainloop_api* m, struct pa_io_event **io_events, unsigned n_io_events) {
    unsigned i;
    struct pa_io_event **ios;
    assert(m && io_events);
    
    for (ios = io_events, i = 0; i < n_io_events; i++, ios++)
        m->io_free(*ios);
    pa_xfree(io_events);
}
