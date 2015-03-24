/***
  This file is part of PulseAudio.

  Copyright 2014 David Henningsson, Canonical Ltd.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "lfe-filter.h"
#include <pulse/xmalloc.h>
#include <pulsecore/filter/biquad.h>
#include <pulsecore/filter/crossover.h>

/* An LR4 filter, implemented as a chain of two Butterworth filters.

   Currently the channel map is fixed so that a highpass filter is applied to all
   channels except for the LFE channel, where a lowpass filter is applied.
   This works well for e g stereo to 2.1/5.1/7.1 scenarios, where the remap engine
   has calculated the LFE channel to be the average of all source channels.
*/

struct pa_lfe_filter {
    float crossover;
    pa_channel_map cm;
    pa_sample_spec ss;
    bool active;
    struct lr4 lr4[PA_CHANNELS_MAX];
};

pa_lfe_filter_t * pa_lfe_filter_new(const pa_sample_spec* ss, const pa_channel_map* cm, float crossover_freq) {

    pa_lfe_filter_t *f = pa_xnew0(struct pa_lfe_filter, 1);
    f->crossover = crossover_freq;
    f->cm = *cm;
    f->ss = *ss;
    pa_lfe_filter_update_rate(f, ss->rate);
    return f;
}

void pa_lfe_filter_free(pa_lfe_filter_t *f) {
    pa_xfree(f);
}

void pa_lfe_filter_reset(pa_lfe_filter_t *f) {
    pa_lfe_filter_update_rate(f, f->ss.rate);
}

pa_memchunk * pa_lfe_filter_process(pa_lfe_filter_t *f, pa_memchunk *buf) {
    int samples = buf->length / pa_frame_size(&f->ss);

    if (!f->active)
        return buf;
    if (f->ss.format == PA_SAMPLE_FLOAT32NE) {
        int i;
        float *data = pa_memblock_acquire_chunk(buf);
        for (i = 0; i < f->cm.channels; i++)
            lr4_process_float32(&f->lr4[i], samples, f->cm.channels, &data[i], &data[i]);
        pa_memblock_release(buf->memblock);
    }
    else if (f->ss.format == PA_SAMPLE_S16NE) {
        int i;
        short *data = pa_memblock_acquire_chunk(buf);
        for (i = 0; i < f->cm.channels; i++)
            lr4_process_s16(&f->lr4[i], samples, f->cm.channels, &data[i], &data[i]);
        pa_memblock_release(buf->memblock);
    }
    else pa_assert_not_reached();
    return buf;
}

void pa_lfe_filter_update_rate(pa_lfe_filter_t *f, uint32_t new_rate) {
    int i;
    float biquad_freq = f->crossover / (new_rate / 2);

    f->ss.rate = new_rate;
    if (biquad_freq <= 0 || biquad_freq >= 1) {
        pa_log_warn("Crossover frequency (%f) outside range for sample rate %d", f->crossover, new_rate);
        f->active = false;
        return;
    }

    for (i = 0; i < f->cm.channels; i++)
        lr4_set(&f->lr4[i], f->cm.map[i] == PA_CHANNEL_POSITION_LFE ? BQ_LOWPASS : BQ_HIGHPASS, biquad_freq);

    f->active = true;
}
