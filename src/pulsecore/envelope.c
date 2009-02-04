/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#include <pulse/sample.h>
#include <pulse/xmalloc.h>

#include <pulsecore/endianmacros.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/macro.h>
#include <pulsecore/flist.h>
#include <pulsecore/semaphore.h>
#include <pulsecore/g711.h>

#include "envelope.h"

/*
    Envelope subsystem for applying linear interpolated volume
    envelopes on audio data. If multiple enevelopes shall be applied
    at the same time, the "minimum" envelope is determined and
    applied.

    Envelopes are defined in a statically allocated constant structure
    pa_envelope_def. It may be activated using pa_envelope_add(). And
    already active envelope may be replaced with pa_envelope_replace()
    and removed with pa_envelope_remove().The combined "minimum"
    envelope can be applied to audio data with pa_envelope_apply().

    _apply() on one hand and _add()/_replace()/_remove() on the other
    can be executed in seperate threads, in which case no locking is
    used.
*/

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

struct pa_envelope_item {
    PA_LLIST_FIELDS(pa_envelope_item);
    const pa_envelope_def *def;
    pa_usec_t start_x;
    union {
        int32_t i;
        float f;
    } start_y;
    unsigned j;
};

enum envelope_state {
    STATE_VALID0,
    STATE_VALID1,
    STATE_READ0,
    STATE_READ1,
    STATE_WAIT0,
    STATE_WAIT1,
    STATE_WRITE0,
    STATE_WRITE1
};

struct pa_envelope {
    pa_sample_spec sample_spec;

    PA_LLIST_HEAD(pa_envelope_item, items);

    pa_atomic_t state;

    size_t x;

    struct {
        unsigned n_points, n_allocated, n_current;

        size_t *x;
        union {
            int32_t *i;
            float *f;
        } y;

        size_t cached_dx;
        int32_t cached_dy_i;
        float cached_dy_dx;
        pa_bool_t cached_valid;
    } points[2];

    pa_bool_t is_float;

    pa_semaphore *semaphore;
};

pa_envelope *pa_envelope_new(const pa_sample_spec *ss) {
    pa_envelope *e;
    pa_assert(ss);

    e = pa_xnew(pa_envelope, 1);

    e->sample_spec = *ss;
    PA_LLIST_HEAD_INIT(pa_envelope_item, e->items);

    e->x = 0;

    e->points[0].n_points = e->points[1].n_points = 0;
    e->points[0].n_allocated = e->points[1].n_allocated = 0;
    e->points[0].n_current = e->points[1].n_current = 0;
    e->points[0].x = e->points[1].x = NULL;
    e->points[0].y.i = e->points[1].y.i = NULL;
    e->points[0].cached_valid = e->points[1].cached_valid = FALSE;

    pa_atomic_store(&e->state, STATE_VALID0);

    e->is_float =
        ss->format == PA_SAMPLE_FLOAT32LE ||
        ss->format == PA_SAMPLE_FLOAT32BE;

    e->semaphore = pa_semaphore_new(0);

    return e;
}

void pa_envelope_free(pa_envelope *e) {
    pa_assert(e);

    while (e->items)
        pa_envelope_remove(e, e->items);

    pa_xfree(e->points[0].x);
    pa_xfree(e->points[1].x);
    pa_xfree(e->points[0].y.i);
    pa_xfree(e->points[1].y.i);

    pa_semaphore_free(e->semaphore);

    pa_xfree(e);
}

static int32_t linear_interpolate_int(pa_usec_t x1, int32_t _y1, pa_usec_t x2, int32_t y2, pa_usec_t x3) {
    return (int32_t) ((double) _y1 + (double) (x3 - x1) * (double) (y2 - _y1) / (double) (x2 - x1));
}

static float linear_interpolate_float(pa_usec_t x1, float _y1, pa_usec_t x2, float y2, pa_usec_t x3) {
    return _y1 + ((float) x3 - (float) x1) * (y2 - _y1) / ((float) x2 - (float) x1);
}

static int32_t item_get_int(pa_envelope_item *i, pa_usec_t x) {
    pa_assert(i);

    if (x <= i->start_x)
        return i->start_y.i;

    x -= i->start_x;

    if (x <= i->def->points_x[0])
        return linear_interpolate_int(0, i->start_y.i,
                                      i->def->points_x[0], i->def->points_y.i[0], x);

    if (x >= i->def->points_x[i->def->n_points-1])
        return i->def->points_y.i[i->def->n_points-1];

    pa_assert(i->j > 0);
    pa_assert(i->def->points_x[i->j-1] <= x);
    pa_assert(x < i->def->points_x[i->j]);

    return linear_interpolate_int(i->def->points_x[i->j-1], i->def->points_y.i[i->j-1],
                                  i->def->points_x[i->j], i->def->points_y.i[i->j], x);
}

static float item_get_float(pa_envelope_item *i, pa_usec_t x) {
    pa_assert(i);

    if (x <= i->start_x)
        return i->start_y.f;

    x -= i->start_x;

    if (x <= i->def->points_x[0])
        return linear_interpolate_float(0, i->start_y.f,
                                        i->def->points_x[0], i->def->points_y.f[0], x);

    if (x >= i->def->points_x[i->def->n_points-1])
        return i->def->points_y.f[i->def->n_points-1];

    pa_assert(i->j > 0);
    pa_assert(i->def->points_x[i->j-1] <= x);
    pa_assert(x < i->def->points_x[i->j]);

    return linear_interpolate_float(i->def->points_x[i->j-1], i->def->points_y.f[i->j-1],
                                    i->def->points_x[i->j], i->def->points_y.f[i->j], x);
}

static void envelope_begin_write(pa_envelope *e, int *v) {
    enum envelope_state new_state, old_state;
    pa_bool_t wait_sem;

    pa_assert(e);
    pa_assert(v);

    for (;;) {
        do {
            wait_sem = FALSE;
            old_state = pa_atomic_load(&e->state);

            switch (old_state) {
                case STATE_VALID0:
                    *v = 1;
                    new_state = STATE_WRITE0;
                    break;
                case STATE_VALID1:
                    *v = 0;
                    new_state = STATE_WRITE1;
                    break;
                case STATE_READ0:
                    new_state = STATE_WAIT0;
                    wait_sem = TRUE;
                    break;
                case STATE_READ1:
                    new_state = STATE_WAIT1;
                    wait_sem = TRUE;
                    break;
                default:
                    pa_assert_not_reached();
            }
        } while (!pa_atomic_cmpxchg(&e->state, old_state, new_state));

        if (!wait_sem)
            break;

        pa_semaphore_wait(e->semaphore);
    }
}

static pa_bool_t envelope_commit_write(pa_envelope *e, int v) {
    enum envelope_state new_state, old_state;

    pa_assert(e);

    do {
        old_state = pa_atomic_load(&e->state);

        switch (old_state) {
            case STATE_WRITE0:
                pa_assert(v == 1);
                new_state = STATE_VALID1;
                break;
            case STATE_WRITE1:
                pa_assert(v == 0);
                new_state = STATE_VALID0;
                break;
            case STATE_VALID0:
            case STATE_VALID1:
            case STATE_READ0:
            case STATE_READ1:
                return FALSE;
            default:
                pa_assert_not_reached();
        }
    } while (!pa_atomic_cmpxchg(&e->state, old_state, new_state));

    return TRUE;
}

static void envelope_begin_read(pa_envelope *e, int *v) {
    enum envelope_state new_state, old_state;
    pa_assert(e);
    pa_assert(v);

    do {
        old_state = pa_atomic_load(&e->state);

        switch (old_state) {
            case STATE_VALID0:
            case STATE_WRITE0:
                *v = 0;
                new_state = STATE_READ0;
                break;
            case STATE_VALID1:
            case STATE_WRITE1:
                *v = 1;
                new_state = STATE_READ1;
                break;
            default:
                pa_assert_not_reached();
        }
    } while (!pa_atomic_cmpxchg(&e->state, old_state, new_state));
}

static void envelope_commit_read(pa_envelope *e, int v) {
    enum envelope_state new_state, old_state;
    pa_bool_t post_sem;

    pa_assert(e);

    do {
        post_sem = FALSE;
        old_state = pa_atomic_load(&e->state);

        switch (old_state) {
            case STATE_READ0:
                pa_assert(v == 0);
                new_state = STATE_VALID0;
                break;
            case STATE_READ1:
                pa_assert(v == 1);
                new_state = STATE_VALID1;
                break;
            case STATE_WAIT0:
                pa_assert(v == 0);
                new_state = STATE_VALID0;
                post_sem = TRUE;
                break;
            case STATE_WAIT1:
                pa_assert(v == 1);
                new_state = STATE_VALID1;
                post_sem = TRUE;
                break;
            default:
                pa_assert_not_reached();
        }
    } while (!pa_atomic_cmpxchg(&e->state, old_state, new_state));

    if (post_sem)
        pa_semaphore_post(e->semaphore);
}

static void envelope_merge(pa_envelope *e, int v) {

    e->points[v].n_points = 0;

    if (e->items) {
        pa_envelope_item *i;
        pa_usec_t x = (pa_usec_t) -1;

        for (i = e->items; i; i = i->next)
            i->j = 0;

        for (;;) {
            pa_bool_t min_is_set;
            pa_envelope_item *s = NULL;

            /* Let's find the next spot on the X axis to analyze */
            for (i = e->items; i; i = i->next) {

                for (;;) {

                    if (i->j >= i->def->n_points)
                        break;

                    if ((x != (pa_usec_t) -1) && i->start_x + i->def->points_x[i->j] <= x) {
                        i->j++;
                        continue;
                    }

                    if (!s || (i->start_x + i->def->points_x[i->j] < s->start_x + s->def->points_x[s->j]))
                        s = i;

                    break;
                }
            }

            if (!s)
                break;

            if (e->points[v].n_points >= e->points[v].n_allocated) {
                e->points[v].n_allocated = PA_MAX(e->points[v].n_points*2, PA_ENVELOPE_POINTS_MAX);

                e->points[v].x = pa_xrealloc(e->points[v].x, sizeof(size_t) * e->points[v].n_allocated);
                e->points[v].y.i = pa_xrealloc(e->points[v].y.i, sizeof(int32_t) * e->points[v].n_allocated);
            }

            x = s->start_x + s->def->points_x[s->j];
            e->points[v].x[e->points[v].n_points] = pa_usec_to_bytes(x, &e->sample_spec);

            min_is_set = FALSE;

            /* Now let's find the lowest value */
            if (e->is_float) {
                float min_f;

                for (i = e->items; i; i = i->next) {
                    float f = item_get_float(i, x);
                    if (!min_is_set || f < min_f) {
                        min_f = f;
                        min_is_set = TRUE;
                    }
                }

                e->points[v].y.f[e->points[v].n_points] = min_f;
            } else {
                int32_t min_k;

                for (i = e->items; i; i = i->next) {
                    int32_t k = item_get_int(i, x);
                    if (!min_is_set || k < min_k) {
                        min_k = k;
                        min_is_set = TRUE;
                    }
                }

                e->points[v].y.i[e->points[v].n_points] = min_k;
            }

            pa_assert_se(min_is_set);
            e->points[v].n_points++;
        }
    }

    e->points[v].n_current = 0;
    e->points[v].cached_valid = FALSE;
}

pa_envelope_item *pa_envelope_add(pa_envelope *e, const pa_envelope_def *def) {
    pa_envelope_item *i;
    int v;

    pa_assert(e);
    pa_assert(def);
    pa_assert(def->n_points > 0);

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        i = pa_xnew(pa_envelope_item, 1);

    i->def = def;

    if (e->is_float)
        i->start_y.f = def->points_y.f[0];
    else
        i->start_y.i = def->points_y.i[0];

    PA_LLIST_PREPEND(pa_envelope_item, e->items, i);

    envelope_begin_write(e, &v);

    do {

        i->start_x = pa_bytes_to_usec(e->x, &e->sample_spec);
        envelope_merge(e, v);

    } while (!envelope_commit_write(e, v));

    return i;
}

pa_envelope_item *pa_envelope_replace(pa_envelope *e, pa_envelope_item *i, const pa_envelope_def *def) {
    pa_usec_t x;
    int v;

    pa_assert(e);
    pa_assert(i);
    pa_assert(def->n_points > 0);

    envelope_begin_write(e, &v);

    for (;;) {
        float saved_f;
        int32_t saved_i;
        uint64_t saved_start_x;
        const pa_envelope_def *saved_def;

        x = pa_bytes_to_usec(e->x, &e->sample_spec);

        if (e->is_float) {
            saved_f = i->start_y.f;
            i->start_y.f = item_get_float(i, x);
        } else {
            saved_i = i->start_y.i;
            i->start_y.i = item_get_int(i, x);
        }

        saved_start_x = i->start_x;
        saved_def = i->def;

        i->start_x = x;
        i->def = def;

        envelope_merge(e, v);

        if (envelope_commit_write(e, v))
            break;

        i->start_x = saved_start_x;
        i->def = saved_def;

        if (e->is_float)
            i->start_y.f = saved_f;
        else
            i->start_y.i = saved_i;
    }

    return i;
}

void pa_envelope_remove(pa_envelope *e, pa_envelope_item *i) {
    int v;

    pa_assert(e);
    pa_assert(i);

    PA_LLIST_REMOVE(pa_envelope_item, e->items, i);

    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);

    envelope_begin_write(e, &v);
    do {
        envelope_merge(e, v);
    } while (!envelope_commit_write(e, v));
}

static int32_t linear_get_int(pa_envelope *e, int v) {
    pa_assert(e);

    /* The repeated division could be replaced by Bresenham, as an
     * optimization */

    if (e->x < e->points[v].x[0])
        return e->points[v].y.i[0];

    for (;;) {
        if (e->points[v].n_current+1 >= e->points[v].n_points)
            return e->points[v].y.i[e->points[v].n_points-1];

        if (e->x < e->points[v].x[e->points[v].n_current+1])
            break;

        e->points[v].n_current++;
        e->points[v].cached_valid = FALSE;
    }

    if (!e->points[v].cached_valid) {
        e->points[v].cached_dx = e->points[v].x[e->points[v].n_current+1] - e->points[v].x[e->points[v].n_current];
        e->points[v].cached_dy_i = e->points[v].y.i[e->points[v].n_current+1] - e->points[v].y.i[e->points[v].n_current];
        e->points[v].cached_valid = TRUE;
    }

    return e->points[v].y.i[e->points[v].n_current] + (e->points[v].cached_dy_i * (int32_t) (e->x - e->points[v].x[e->points[v].n_current])) / (int32_t) e->points[v].cached_dx;
}

static float linear_get_float(pa_envelope *e, int v) {
    pa_assert(e);

    if (e->x < e->points[v].x[0])
        return e->points[v].y.f[0];

    for (;;) {
        if (e->points[v].n_current+1 >= e->points[v].n_points)
            return e->points[v].y.f[e->points[v].n_points-1];

        if (e->x < e->points[v].x[e->points[v].n_current+1])
            break;

        e->points[v].n_current++;
        e->points[v].cached_valid = FALSE;
    }

    if (!e->points[v].cached_valid) {
        e->points[v].cached_dy_dx =
            (e->points[v].y.f[e->points[v].n_current+1] - e->points[v].y.f[e->points[v].n_current]) /
            ((float) e->points[v].x[e->points[v].n_current+1] - (float) e->points[v].x[e->points[v].n_current]);
        e->points[v].cached_valid = TRUE;
    }

    return e->points[v].y.f[e->points[v].n_current] + (float) (e->x - e->points[v].x[e->points[v].n_current]) * e->points[v].cached_dy_dx;
}

void pa_envelope_apply(pa_envelope *e, pa_memchunk *chunk) {
    int v;

    pa_assert(e);
    pa_assert(chunk);

    envelope_begin_read(e, &v);

    if (e->points[v].n_points > 0) {
        void *p;
        size_t fs, n;

        pa_memchunk_make_writable(chunk, 0);
        p = (uint8_t*) pa_memblock_acquire(chunk->memblock) + chunk->index;
        fs = pa_frame_size(&e->sample_spec);
        n = chunk->length;

        switch (e->sample_spec.format) {

            case PA_SAMPLE_U8: {
                uint8_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++)
                        *t = (uint8_t) (((factor * ((int16_t) *t - 0x80)) / 0x10000) + 0x80);
                }

                break;
            }

            case PA_SAMPLE_ULAW: {
                uint8_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++) {
                        int16_t k = st_ulaw2linear16(*t);
                        *t = (uint8_t) st_14linear2ulaw((int16_t) (((factor * k) / 0x10000) >> 2));
                    }
                }

                break;
            }

            case PA_SAMPLE_ALAW: {
                uint8_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++) {
                        int16_t k = st_alaw2linear16(*t);
                        *t = (uint8_t) st_13linear2alaw((int16_t) (((factor * k) / 0x10000) >> 3));
                    }
                }

                break;
            }

            case PA_SAMPLE_S16NE: {
                int16_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++)
                        *t = (int16_t) ((factor * *t) / 0x10000);
                }

                break;
            }

            case PA_SAMPLE_S16RE: {
                int16_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++) {
                        int16_t r = (int16_t) ((factor * PA_INT16_SWAP(*t)) / 0x10000);
                        *t = PA_INT16_SWAP(r);
                    }
                }

                break;
            }

            case PA_SAMPLE_S32NE: {
                int32_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++)
                        *t = (int32_t) (((int64_t) factor * (int64_t) *t) / 0x10000);
                }

                break;
            }

            case PA_SAMPLE_S32RE: {
                int32_t *t;

                for (t = p; n > 0; n -= fs) {
                    int32_t factor = linear_get_int(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++) {
                        int32_t r = (int32_t) (((int64_t) factor * (int64_t) PA_INT32_SWAP(*t)) / 0x10000);
                        *t = PA_INT32_SWAP(r);
                    }
                }

                break;
            }

            case PA_SAMPLE_FLOAT32NE: {
                float *t;

                for (t = p; n > 0; n -= fs) {
                    float factor = linear_get_float(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++)
                        *t = *t * factor;
                }

                break;
            }

            case PA_SAMPLE_FLOAT32RE: {
                float *t;

                for (t = p; n > 0; n -= fs) {
                    float factor = linear_get_float(e, v);
                    unsigned c;
                    e->x += fs;

                    for (c = 0; c < e->sample_spec.channels; c++, t++) {
                        float r = PA_FLOAT32_SWAP(*t) * factor;
                        *t = PA_FLOAT32_SWAP(r);
                    }
                }

                break;
            }

            case PA_SAMPLE_S24LE:
            case PA_SAMPLE_S24BE:
            case PA_SAMPLE_S24_32LE:
            case PA_SAMPLE_S24_32BE:
                /* FIXME */
                pa_assert_not_reached();

            case PA_SAMPLE_MAX:
            case PA_SAMPLE_INVALID:
                pa_assert_not_reached();
        }

        pa_memblock_release(chunk->memblock);

        e->x += chunk->length;
    } else {
        /* When we have no envelope to apply we reset our origin */
        e->x = 0;
    }

    envelope_commit_read(e, v);
}

void pa_envelope_rewind(pa_envelope *e, size_t n_bytes) {
    int v;

    pa_assert(e);

    envelope_begin_read(e, &v);

    if (n_bytes < e->x)
        e->x -= n_bytes;
    else
        e->x = 0;

    e->points[v].n_current = 0;
    e->points[v].cached_valid = FALSE;

    envelope_commit_read(e, v);
}
