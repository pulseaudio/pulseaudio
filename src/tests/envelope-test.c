/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include <pulse/sample.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>

#include <pulsecore/envelope.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/memblock.h>
#include <pulsecore/sample-util.h>

const pa_envelope_def ramp_down = {
    .n_points = 2,
    .points_x = { 100*PA_USEC_PER_MSEC, 300*PA_USEC_PER_MSEC },
    .points_y = {
        .f = { 1.0f, 0.2f },
        .i = { 0x10000, 0x10000/5 }
    }
};

const pa_envelope_def ramp_up = {
    .n_points = 2,
    .points_x = { 100*PA_USEC_PER_MSEC, 300*PA_USEC_PER_MSEC },
    .points_y = {
        .f = { 0.2f, 1.0f },
        .i = { 0x10000/5, 0x10000 }
    }
};

const pa_envelope_def ramp_down2 = {
    .n_points = 2,
    .points_x = { 50*PA_USEC_PER_MSEC, 900*PA_USEC_PER_MSEC },
    .points_y = {
        .f = { 0.8f, 0.7f },
        .i = { 0x10000*4/5, 0x10000*7/10 }
    }
};

const pa_envelope_def ramp_up2 = {
    .n_points = 2,
    .points_x = { 50*PA_USEC_PER_MSEC, 900*PA_USEC_PER_MSEC },
    .points_y = {
        .f = { 0.7f, 0.9f },
        .i = { 0x10000*7/10, 0x10000*9/10 }
    }
};

static void dump_block(const pa_sample_spec *ss, const pa_memchunk *chunk) {
    void *d;
    unsigned i;

    static unsigned j = 0;

    d = pa_memblock_acquire(chunk->memblock);

    switch (ss->format) {

        case PA_SAMPLE_U8:
        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW: {
            uint8_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("0x%02x ", *(u++));

            break;
        }

        case PA_SAMPLE_S16NE:
        case PA_SAMPLE_S16RE: {
            int16_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("%i\t%i\n", j++, *(u++));

            break;
        }

        case PA_SAMPLE_S32NE:
        case PA_SAMPLE_S32RE: {
            int32_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("%i\t%i\n", j++, *(u++));

            break;
        }

        case PA_SAMPLE_FLOAT32NE:
        case PA_SAMPLE_FLOAT32RE: {
            float *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++) {
                printf("%i\t%1.3g\n", j++, PA_MAYBE_FLOAT32_SWAP(ss->format == PA_SAMPLE_FLOAT32RE, *u));
                u++;
            }

            break;
        }

        default:
            pa_assert_not_reached();
    }

    printf("\n");

    pa_memblock_release(chunk->memblock);
}

static pa_memblock * generate_block(pa_mempool *pool, const pa_sample_spec *ss) {
    pa_memblock *block;
    void *d;
    unsigned n_samples;

    block = pa_memblock_new(pool, pa_bytes_per_second(ss));
    n_samples = (unsigned) (pa_memblock_get_length(block) / pa_sample_size(ss));

    d = pa_memblock_acquire(block);

    switch (ss->format) {

        case PA_SAMPLE_S16NE:
        case PA_SAMPLE_S16RE: {
            int16_t *i;

            for (i = d; n_samples > 0; n_samples--, i++)
                *i = 0x7FFF;

            break;
        }

        case PA_SAMPLE_S32NE:
        case PA_SAMPLE_S32RE: {
            int32_t *i;

            for (i = d; n_samples > 0; n_samples--, i++)
                *i = 0x7FFFFFFF;

            break;
        }

        case PA_SAMPLE_FLOAT32RE:
        case PA_SAMPLE_FLOAT32NE: {
            float *f;

            for (f = d; n_samples > 0; n_samples--, f++)
                *f = PA_MAYBE_FLOAT32_SWAP(ss->format == PA_SAMPLE_FLOAT32RE, 1.0f);

            break;
        }

        default:
            pa_assert_not_reached();
    }

    pa_memblock_release(block);
    return block;
}

int main(int argc, char *argv[]) {
    pa_mempool *pool;
    pa_memblock *block;
    pa_memchunk chunk;
    pa_envelope *envelope;
    pa_envelope_item *item1, *item2;

    const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16NE,
        .channels = 1,
        .rate = 200
    };

    const pa_cvolume v = {
        .channels = 1,
        .values = { PA_VOLUME_NORM, PA_VOLUME_NORM/2 }
    };

    pa_log_set_level(PA_LOG_DEBUG);

    pa_assert_se(pool = pa_mempool_new(FALSE, 0));
    pa_assert_se(envelope = pa_envelope_new(&ss));

    block = generate_block(pool, &ss);

    chunk.memblock = pa_memblock_ref(block);
    chunk.length = pa_memblock_get_length(block);
    chunk.index = 0;

    pa_volume_memchunk(&chunk, &ss, &v);

    item1 = pa_envelope_add(envelope, &ramp_down);
    item2 = pa_envelope_add(envelope, &ramp_down2);
    pa_envelope_apply(envelope, &chunk);
    dump_block(&ss, &chunk);

    pa_memblock_unref(chunk.memblock);

    chunk.memblock = pa_memblock_ref(block);
    chunk.length = pa_memblock_get_length(block);
    chunk.index = 0;

    item1 = pa_envelope_replace(envelope, item1, &ramp_up);
    item2 = pa_envelope_replace(envelope, item2, &ramp_up2);
    pa_envelope_apply(envelope, &chunk);
    dump_block(&ss, &chunk);

    pa_memblock_unref(chunk.memblock);

    pa_envelope_remove(envelope, item1);
    pa_envelope_remove(envelope, item2);
    pa_envelope_free(envelope);

    pa_memblock_unref(block);

    pa_mempool_free(pool);

    return 0;
}
