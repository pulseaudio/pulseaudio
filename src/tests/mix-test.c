/* $Id: resampler-test.c 2037 2007-11-09 02:45:07Z lennart $ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <pulse/sample.h>
#include <pulse/volume.h>

#include <pulsecore/resampler.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/memblock.h>
#include <pulsecore/sample-util.h>

#include <liboil/liboil.h>

static float swap_float(float a) {
    uint32_t *b = (uint32_t*) &a;
    *b = PA_UINT32_SWAP(*b);
    return a;
}

static void dump_block(const pa_sample_spec *ss, const pa_memchunk *chunk) {
    void *d;
    unsigned i;

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
            uint16_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("0x%04x ", *(u++));

            break;
        }

        case PA_SAMPLE_S32NE:
        case PA_SAMPLE_S32RE: {
            uint32_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("0x%08x ", *(u++));

            break;
        }

        case PA_SAMPLE_FLOAT32NE:
        case PA_SAMPLE_FLOAT32RE: {
            float *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++) {
                printf("%1.5f ",  ss->format == PA_SAMPLE_FLOAT32NE ? *u : swap_float(*u));
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

static pa_memblock* generate_block(pa_mempool *pool, const pa_sample_spec *ss) {
    pa_memblock *r;
    void *d;
    unsigned i;

    pa_assert_se(r = pa_memblock_new(pool, pa_frame_size(ss) * 10));
    d = pa_memblock_acquire(r);

    switch (ss->format) {

        case PA_SAMPLE_U8:
        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW: {
            uint8_t *u = d;

            u[0] = 0x00;
            u[1] = 0xFF;
            u[2] = 0x7F;
            u[3] = 0x80;
            u[4] = 0x9f;
            u[5] = 0x3f;
            u[6] = 0x1;
            u[7] = 0xF0;
            u[8] = 0x20;
            u[9] = 0x21;
            break;
        }

        case PA_SAMPLE_S16NE:
        case PA_SAMPLE_S16RE: {
            uint16_t *u = d;

            u[0] = 0x0000;
            u[1] = 0xFFFF;
            u[2] = 0x7FFF;
            u[3] = 0x8000;
            u[4] = 0x9fff;
            u[5] = 0x3fff;
            u[6] = 0x1;
            u[7] = 0xF000;
            u[8] = 0x20;
            u[9] = 0x21;
            break;
        }

        case PA_SAMPLE_S32NE:
        case PA_SAMPLE_S32RE: {
            uint32_t *u = d;

            u[0] = 0x00000001;
            u[1] = 0xFFFF0002;
            u[2] = 0x7FFF0003;
            u[3] = 0x80000004;
            u[4] = 0x9fff0005;
            u[5] = 0x3fff0006;
            u[6] =    0x10007;
            u[7] = 0xF0000008;
            u[8] =   0x200009;
            u[9] =   0x21000A;
            break;
        }

        case PA_SAMPLE_FLOAT32NE:
        case PA_SAMPLE_FLOAT32RE: {
            float *u = d;

            u[0] = 0.0;
            u[1] = -1.0;
            u[2] = 1.0;
            u[3] = 4711;
            u[4] = 0.222;
            u[5] = 0.33;
            u[6] = -.3;
            u[7] = 99;
            u[8] = -0.555;
            u[9] = -.123;

            if (ss->format == PA_SAMPLE_FLOAT32RE)
                for (i = 0; i < 10; i++)
                    u[i] = swap_float(u[i]);

            break;
        }

        default:
            pa_assert_not_reached();
    }

    pa_memblock_release(r);

    return r;
}

int main(int argc, char *argv[]) {
    pa_mempool *pool;
    pa_sample_spec a;
    pa_cvolume v;

    oil_init();
    pa_log_set_maximal_level(PA_LOG_DEBUG);

    pa_assert_se(pool = pa_mempool_new(FALSE));

    a.channels = 1;
    a.rate = 44100;

    v.channels = a.channels;
    v.values[0] = pa_sw_volume_from_linear(0.9);

    for (a.format = 0; a.format < PA_SAMPLE_MAX; a.format ++) {
        pa_memchunk i, j, k;
        pa_mix_info m[2];
        void *ptr;

        printf("=== mixing: %s\n", pa_sample_format_to_string(a.format));

        /* Generate block */
        i.memblock = generate_block(pool, &a);
        i.length = pa_memblock_get_length(i.memblock);
        i.index = 0;

        /* Make a copy */
        j = i;
        pa_memblock_ref(j.memblock);
        pa_memchunk_make_writable(&j, 0);

        /* Adjust volume of the copy */
        pa_volume_memchunk(&j, &a, &v);

        m[0].chunk = i;
        m[0].volume.values[0] = PA_VOLUME_NORM;
        m[0].volume.channels = a.channels;
        m[1].chunk = j;
        m[1].volume.values[0] = PA_VOLUME_NORM;
        m[1].volume.channels = a.channels;

        k.memblock = pa_memblock_new(pool, i.length);
        k.length = i.length;
        k.index = 0;

        ptr = (uint8_t*) pa_memblock_acquire(k.memblock) + k.index;
        pa_mix(m, 2, ptr, k.length, &a, NULL, FALSE);
        pa_memblock_release(k.memblock);

        dump_block(&a, &i);
        dump_block(&a, &j);
        dump_block(&a, &k);

        pa_memblock_unref(i.memblock);
        pa_memblock_unref(j.memblock);
        pa_memblock_unref(k.memblock);
    }

    pa_mempool_free(pool);

    return 0;
}
