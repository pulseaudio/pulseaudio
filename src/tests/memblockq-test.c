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
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <check.h>

#include <pulsecore/memblockq.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/core-util.h>

#include <pulse/xmalloc.h>

static const char *fixed[] = {
    "1122444411441144__22__11______3333______________________________",
    "__________________3333__________________________________________"
};
static const char *manual[] = {
    "1122444411441144__22__11______3333______________________________",
    "__________________3333______________________________"
};

/*
 * utility function to create a memchunk
 */
static pa_memchunk memchunk_from_str(pa_mempool *p, const char* data)
{
    pa_memchunk res;
    size_t size = strlen(data);

    res.memblock = pa_memblock_new_fixed(p, (void*)data, size, true);
    ck_assert_ptr_ne(res.memblock, NULL);

    res.index = 0;
    res.length = pa_memblock_get_length(res.memblock);

    return res;
}

static void dump_chunk(const pa_memchunk *chunk, pa_strbuf *buf) {
    size_t n;
    void *q;
    char *e;

    fail_unless(chunk != NULL);

    q = pa_memblock_acquire(chunk->memblock);
    for (e = (char*) q + chunk->index, n = 0; n < chunk->length; n++, e++) {
        fprintf(stderr, "%c", *e);
        pa_strbuf_putc(buf, *e);
    }
    pa_memblock_release(chunk->memblock);
}

static void dump(pa_memblockq *bq, int n) {
    pa_memchunk out;
    pa_strbuf *buf;
    char *str;

    pa_assert(bq);

    /* First let's dump this as fixed block */
    fprintf(stderr, "FIXED >");
    pa_memblockq_peek_fixed_size(bq, 64, &out);
    buf = pa_strbuf_new();
    dump_chunk(&out, buf);
    pa_memblock_unref(out.memblock);
    str = pa_strbuf_to_string_free(buf);
    fail_unless(pa_streq(str, fixed[n]));
    pa_xfree(str);
    fprintf(stderr, "<\n");

    /* Then let's dump the queue manually */
    fprintf(stderr, "MANUAL>");

    buf = pa_strbuf_new();
    for (;;) {
        if (pa_memblockq_peek(bq, &out) < 0)
            break;

        dump_chunk(&out, buf);
        pa_memblock_unref(out.memblock);
        pa_memblockq_drop(bq, out.length);
    }
    str = pa_strbuf_to_string_free(buf);
    fail_unless(pa_streq(str, manual[n]));
    pa_xfree(str);
    fprintf(stderr, "<\n");
}

/*
 * utility function to validate invariants
 *
 * The different values like base, maxlength etc follow certain rules.
 * This convenience function makes sure that changes don't violate
 * these rules.
 */
static void check_queue_invariants(pa_memblockq *bq) {
    size_t base = pa_memblockq_get_base(bq);
    size_t maxlength = pa_memblockq_get_maxlength(bq);
    size_t tlength = pa_memblockq_get_tlength(bq);
    size_t minreq = pa_memblockq_get_minreq(bq);
    size_t prebuf = pa_memblockq_get_prebuf(bq);
    size_t length = pa_memblockq_get_length(bq);
    size_t missing = pa_memblockq_missing(bq);

    /* base > zero */
    ck_assert_int_gt(base, 0);

    /* maxlength multiple of base
     * maxlength >= base */
    ck_assert_int_eq(maxlength % base, 0);
    ck_assert_int_ge(maxlength, base);

    /* tlength multiple of base
     * tlength >= base
     * tlength <= maxlength */
    ck_assert_int_eq(tlength % base, 0);
    ck_assert_int_ge(tlength, base);
    ck_assert_int_le(tlength, maxlength);

    /* minreq multiple of base
     * minreq >= base
     * minreq <= tlength */
    ck_assert_int_eq(minreq % base, 0);
    ck_assert_int_ge(minreq, base);
    ck_assert_int_le(minreq, tlength);

    /* prebuf multiple of base
     * prebuf >= 0
     * prebuf <= tlength + base - minreq
     * prebuf <= tlength (because minreq >= base) */
    ck_assert_int_eq(prebuf % base, 0);
    ck_assert_int_ge(prebuf, 0);
    ck_assert_int_le(prebuf, tlength + base - minreq);
    ck_assert_int_le(prebuf, tlength);

    /* length >= 0
     * length <= maxlength */
    ck_assert_int_ge(length, 0);
    ck_assert_int_le(length, maxlength);

    /* missing >= 0
     * missing <= tlength
     * minimum reported amount of missing data is minreq
     * reported amount of missing data is target length minus actual length */
    ck_assert_int_ge(missing, 0);
    ck_assert_int_le(missing, tlength);
    ck_assert((missing == 0) || (missing >= minreq));
    ck_assert((missing == 0) || (missing == tlength - length));
}

START_TEST (memchunk_from_str_test) {
    pa_mempool *p;
    pa_memchunk chunk;

    p = pa_mempool_new(PA_MEM_TYPE_PRIVATE, 0, true);
    ck_assert_ptr_ne(p, NULL);

    /* allocate memchunk and check default settings */
    chunk = memchunk_from_str(p, "abcd");
    ck_assert_ptr_ne(chunk.memblock, NULL);
    ck_assert_int_eq(chunk.index, 0);
    ck_assert_int_eq(chunk.length, 4);

    /* cleanup */
    pa_memblock_unref(chunk.memblock);
    pa_mempool_unref(p);
}
END_TEST

START_TEST (memblockq_test) {
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

    p = pa_mempool_new(PA_MEM_TYPE_PRIVATE, 0, true);
    ck_assert_ptr_ne(p, NULL);

    silence = memchunk_from_str(p, "__");

    bq = pa_memblockq_new("test memblockq", 0, 200, 10, &ss, 4, 4, 40, &silence);
    fail_unless(bq != NULL);
    check_queue_invariants(bq);

    chunk1 = memchunk_from_str(p, "11");
    chunk2 = memchunk_from_str(p, "XX22");
    chunk2.index += 2;
    chunk2.length -= 2;
    chunk3 = memchunk_from_str(p, "3333");
    chunk4 = memchunk_from_str(p, "44444444");

    ret = pa_memblockq_push(bq, &chunk1);
    fail_unless(ret == 0);

    ret = pa_memblockq_push(bq, &chunk2);
    fail_unless(ret == 0);

    ret = pa_memblockq_push(bq, &chunk3);
    fail_unless(ret == 0);

    ret = pa_memblockq_push(bq, &chunk4);
    fail_unless(ret == 0);

    check_queue_invariants(bq);

    pa_memblockq_seek(bq, -6, 0, true);
    ret = pa_memblockq_push(bq, &chunk3);
    fail_unless(ret == 0);

    pa_memblockq_seek(bq, -2, 0, true);
    ret = pa_memblockq_push(bq, &chunk1);
    fail_unless(ret == 0);

    pa_memblockq_seek(bq, -10, 0, true);
    ret = pa_memblockq_push(bq, &chunk4);
    fail_unless(ret == 0);

    pa_memblockq_seek(bq, 10, 0, true);

    ret = pa_memblockq_push(bq, &chunk1);
    fail_unless(ret == 0);

    pa_memblockq_seek(bq, -6, 0, true);
    ret = pa_memblockq_push(bq, &chunk2);
    fail_unless(ret == 0);

    /* Test splitting */
    pa_memblockq_seek(bq, -12, 0, true);
    ret = pa_memblockq_push(bq, &chunk1);
    fail_unless(ret == 0);

    pa_memblockq_seek(bq, 20, 0, true);

    /* Test merging */
    ret = pa_memblockq_push(bq, &chunk3);
    fail_unless(ret == 0);
    pa_memblockq_seek(bq, -2, 0, true);

    chunk3.index += 2;
    chunk3.length -= 2;
    ret = pa_memblockq_push(bq, &chunk3);
    fail_unless(ret == 0);

    pa_memblockq_seek(bq, 30, PA_SEEK_RELATIVE, true);

    dump(bq, 0);

    pa_memblockq_rewind(bq, 52);

    dump(bq, 1);

    check_queue_invariants(bq);

    pa_memblockq_free(bq);
    pa_memblock_unref(silence.memblock);
    pa_memblock_unref(chunk1.memblock);
    pa_memblock_unref(chunk2.memblock);
    pa_memblock_unref(chunk3.memblock);
    pa_memblock_unref(chunk4.memblock);

    pa_mempool_unref(p);
}
END_TEST

START_TEST (pop_missing_test) {
    int ret;
    size_t missing;

    pa_mempool *p;
    pa_memblockq *bq;
    pa_memchunk chunk;
    char buffer[2048];
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 48000,
        .channels = 1
    };

    pa_log_set_level(PA_LOG_DEBUG);

    bq = pa_memblockq_new("test memblockq", 0, 4096, 2048, &ss, 0, 512, 512, NULL);
    fail_unless(bq != NULL);

    /* Empty buffer, so expect tlength */
    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 2048);

    /* Everything requested, so should be satisfied */
    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 0);

    p = pa_mempool_new(PA_MEM_TYPE_PRIVATE, 0, true);

    chunk.memblock = pa_memblock_new_fixed(p, buffer, sizeof(buffer), 1);
    fail_unless(chunk.memblock != NULL);

    chunk.index = 0;
    chunk.length = sizeof(buffer);

    /* Fill buffer (i.e. satisfy earlier request) */
    ret = pa_memblockq_push(bq, &chunk);
    fail_unless(ret == 0);

    /* Should still be happy */
    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 0);

    /* Check that we don't request less than minreq */
    pa_memblockq_drop(bq, 400);
    missing = pa_memblockq_pop_missing(bq);
    ck_assert_int_eq(missing, 0);

    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 0);

    /* Reduce tlength under what's dropped and under previous minreq */
    pa_memblockq_set_tlength(bq, 256);
    pa_memblockq_set_minreq(bq, 64);

    /* We are now overbuffered and should not request more */
    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 0);

    /* Drop more data so we are below tlength again, but just barely */
    pa_memblockq_drop(bq, 1400);

    /* Should still honour minreq */
    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 0);

    /* Finally drop enough to fall below minreq */
    pa_memblockq_drop(bq, 80);

    /* And expect a request */
    missing = pa_memblockq_pop_missing(bq);
    fail_unless(missing == 88);

    pa_memblockq_free(bq);
    pa_memblock_unref(chunk.memblock);
    pa_mempool_unref(p);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("Memblock Queue");
    tc = tcase_create("memblockq");
    tcase_add_test(tc, memchunk_from_str_test);
    tcase_add_test(tc, memblockq_test);
    tcase_add_test(tc, pop_missing_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
