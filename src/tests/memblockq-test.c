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

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <pulsecore/memblockq.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

static void dump_chunk(const pa_memchunk *chunk) {
    size_t n;
    void *q;
    char *e;

    pa_assert(chunk);

    q = pa_memblock_acquire(chunk->memblock);
    for (e = (char*) q + chunk->index, n = 0; n < chunk->length; n++, e++)
        fprintf(stderr, "%c", *e);
    pa_memblock_release(chunk->memblock);
}

static void dump(pa_memblockq *bq) {
    pa_memchunk out;

    pa_assert(bq);

    /* First let's dump this as fixed block */
    fprintf(stderr, "FIXED >");
    pa_memblockq_peek_fixed_size(bq, 64, &out);
    dump_chunk(&out);
    pa_memblock_unref(out.memblock);
    fprintf(stderr, "<\n");

    /* Then let's dump the queue manually */
    fprintf(stderr, "MANUAL>");

    for (;;) {
        if (pa_memblockq_peek(bq, &out) < 0)
            break;

        dump_chunk(&out);
        pa_memblock_unref(out.memblock);
        pa_memblockq_drop(bq, out.length);
    }

    fprintf(stderr, "<\n");
}

int main(int argc, char *argv[]) {
    int ret;

    pa_mempool *p;
    pa_memblockq *bq;
    pa_memchunk chunk1, chunk2, chunk3, chunk4;
    pa_memchunk silence;
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 48000,
        .channels = 1
    };

    pa_log_set_level(PA_LOG_DEBUG);

    p = pa_mempool_new(FALSE, 0);

    pa_assert_se(silence.memblock = pa_memblock_new_fixed(p, (char*) "__", 2, 1));
    silence.index = 0;
    silence.length = pa_memblock_get_length(silence.memblock);

    pa_assert_se(bq = pa_memblockq_new("test memblockq", 0, 200, 10, &ss, 4, 4, 40, &silence));

    pa_assert_se(chunk1.memblock = pa_memblock_new_fixed(p, (char*) "11", 2, 1));
    chunk1.index = 0;
    chunk1.length = 2;

    pa_assert_se(chunk2.memblock = pa_memblock_new_fixed(p, (char*) "XX22", 4, 1));
    chunk2.index = 2;
    chunk2.length = 2;

    pa_assert_se(chunk3.memblock = pa_memblock_new_fixed(p, (char*) "3333", 4, 1));
    chunk3.index = 0;
    chunk3.length = 4;

    pa_assert_se(chunk4.memblock = pa_memblock_new_fixed(p, (char*) "44444444", 8, 1));
    chunk4.index = 0;
    chunk4.length = 8;

    ret = pa_memblockq_push(bq, &chunk1);
    assert(ret == 0);

    ret = pa_memblockq_push(bq, &chunk2);
    assert(ret == 0);

    ret = pa_memblockq_push(bq, &chunk3);
    assert(ret == 0);

    ret = pa_memblockq_push(bq, &chunk4);
    assert(ret == 0);

    pa_memblockq_seek(bq, -6, 0, TRUE);
    ret = pa_memblockq_push(bq, &chunk3);
    assert(ret == 0);

    pa_memblockq_seek(bq, -2, 0, TRUE);
    ret = pa_memblockq_push(bq, &chunk1);
    assert(ret == 0);

    pa_memblockq_seek(bq, -10, 0, TRUE);
    ret = pa_memblockq_push(bq, &chunk4);
    assert(ret == 0);

    pa_memblockq_seek(bq, 10, 0, TRUE);

    ret = pa_memblockq_push(bq, &chunk1);
    assert(ret == 0);

    pa_memblockq_seek(bq, -6, 0, TRUE);
    ret = pa_memblockq_push(bq, &chunk2);
    assert(ret == 0);

    /* Test splitting */
    pa_memblockq_seek(bq, -12, 0, TRUE);
    ret = pa_memblockq_push(bq, &chunk1);
    assert(ret == 0);

    pa_memblockq_seek(bq, 20, 0, TRUE);

    /* Test merging */
    ret = pa_memblockq_push(bq, &chunk3);
    assert(ret == 0);
    pa_memblockq_seek(bq, -2, 0, TRUE);

    chunk3.index += 2;
    chunk3.length -= 2;
    ret = pa_memblockq_push(bq, &chunk3);
    assert(ret == 0);

    pa_memblockq_seek(bq, 30, PA_SEEK_RELATIVE, TRUE);

    dump(bq);

    pa_memblockq_rewind(bq, 52);

    dump(bq);

    pa_memblockq_free(bq);
    pa_memblock_unref(silence.memblock);
    pa_memblock_unref(chunk1.memblock);
    pa_memblock_unref(chunk2.memblock);
    pa_memblock_unref(chunk3.memblock);
    pa_memblock_unref(chunk4.memblock);

    pa_mempool_free(p);

    return 0;
}
