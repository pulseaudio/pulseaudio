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

#include <pulse/sample.h>
#include <pulse/volume.h>

#include <pulsecore/resampler.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/memblock.h>
#include <pulsecore/sample-util.h>

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
                printf("      0x%02x ", *(u++));

            break;
        }

        case PA_SAMPLE_S16NE:
        case PA_SAMPLE_S16RE: {
            uint16_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("    0x%04x ", *(u++));

            break;
        }

        case PA_SAMPLE_S32NE:
        case PA_SAMPLE_S32RE: {
            uint32_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("0x%08x ", *(u++));

            break;
        }

        case PA_SAMPLE_S24_32NE:
        case PA_SAMPLE_S24_32RE: {
            uint32_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++)
                printf("0x%08x ", *(u++));

            break;
        }

        case PA_SAMPLE_FLOAT32NE:
        case PA_SAMPLE_FLOAT32RE: {
            float *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++) {
                printf("%4.3g ", ss->format == PA_SAMPLE_FLOAT32NE ? *u : PA_FLOAT32_SWAP(*u));
                u++;
            }

            break;
        }

        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE: {
            uint8_t *u = d;

            for (i = 0; i < chunk->length / pa_frame_size(ss); i++) {
                printf("  0x%06x ", PA_READ24NE(u));
                u += pa_frame_size(ss);
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

        case PA_SAMPLE_S24_32NE:
        case PA_SAMPLE_S24_32RE: {
            uint32_t *u = d;

            u[0] = 0x000001;
            u[1] = 0xFF0002;
            u[2] = 0x7F0003;
            u[3] = 0x800004;
            u[4] = 0x9f0005;
            u[5] = 0x3f0006;
            u[6] =    0x107;
            u[7] = 0xF00008;
            u[8] =   0x2009;
            u[9] =   0x210A;
            break;
        }

        case PA_SAMPLE_FLOAT32NE:
        case PA_SAMPLE_FLOAT32RE: {
            float *u = d;

            u[0] = 0.0f;
            u[1] = -1.0f;
            u[2] = 1.0f;
            u[3] = 4711.0f;
            u[4] = 0.222f;
            u[5] = 0.33f;
            u[6] = -.3f;
            u[7] = 99.0f;
            u[8] = -0.555f;
            u[9] = -.123f;

            if (ss->format == PA_SAMPLE_FLOAT32RE)
                for (i = 0; i < 10; i++)
                    u[i] = PA_FLOAT32_SWAP(u[i]);

            break;
        }

        case PA_SAMPLE_S24NE:
        case PA_SAMPLE_S24RE: {
            uint8_t *u = d;

            PA_WRITE24NE(u,    0x000001);
            PA_WRITE24NE(u+3,  0xFF0002);
            PA_WRITE24NE(u+6,  0x7F0003);
            PA_WRITE24NE(u+9,  0x800004);
            PA_WRITE24NE(u+12, 0x9f0005);
            PA_WRITE24NE(u+15, 0x3f0006);
            PA_WRITE24NE(u+18,    0x107);
            PA_WRITE24NE(u+21, 0xF00008);
            PA_WRITE24NE(u+24,   0x2009);
            PA_WRITE24NE(u+27,   0x210A);
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
    pa_sample_spec a, b;
    pa_cvolume v;

    pa_log_set_level(PA_LOG_DEBUG);

    pa_assert_se(pool = pa_mempool_new(FALSE, 0));

    a.channels = b.channels = 1;
    a.rate = b.rate = 44100;

    v.channels = a.channels;
    v.values[0] = pa_sw_volume_from_linear(0.5);

    for (a.format = 0; a.format < PA_SAMPLE_MAX; a.format ++) {
        for (b.format = 0; b.format < PA_SAMPLE_MAX; b.format ++) {
            pa_resampler *forth, *back;
            pa_memchunk i, j, k;

            printf("=== %s -> %s -> %s -> /2\n",
                   pa_sample_format_to_string(a.format),
                   pa_sample_format_to_string(b.format),
                   pa_sample_format_to_string(a.format));

            pa_assert_se(forth = pa_resampler_new(pool, &a, NULL, &b, NULL, PA_RESAMPLER_AUTO, 0));
            pa_assert_se(back = pa_resampler_new(pool, &b, NULL, &a, NULL, PA_RESAMPLER_AUTO, 0));

            i.memblock = generate_block(pool, &a);
            i.length = pa_memblock_get_length(i.memblock);
            i.index = 0;
            pa_resampler_run(forth, &i, &j);
            pa_resampler_run(back, &j, &k);

            printf("before:  ");
            dump_block(&a, &i);
            printf("after :  ");
            dump_block(&b, &j);
            printf("reverse: ");
            dump_block(&a, &k);

            pa_memblock_unref(j.memblock);
            pa_memblock_unref(k.memblock);

            pa_volume_memchunk(&i, &a, &v);
            printf("volume:  ");
            dump_block(&a, &i);

            pa_memblock_unref(i.memblock);

            pa_resampler_free(forth);
            pa_resampler_free(back);
        }
    }

    pa_mempool_free(pool);

    return 0;
}
