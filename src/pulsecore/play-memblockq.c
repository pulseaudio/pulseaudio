/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/gccmacro.h>
#include <pulsecore/thread-mq.h>

#include "play-memblockq.h"

typedef struct memblockq_stream {
    pa_msgobject parent;
    pa_core *core;
    pa_sink_input *sink_input;
    pa_memblockq *memblockq;
} memblockq_stream;

enum {
    MEMBLOCKQ_STREAM_MESSAGE_UNLINK,
};

PA_DECLARE_CLASS(memblockq_stream);
#define MEMBLOCKQ_STREAM(o) (memblockq_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(memblockq_stream, pa_msgobject);

static void memblockq_stream_unlink(memblockq_stream *u) {
    pa_assert(u);

    if (!u->sink_input)
        return;

    pa_sink_input_unlink(u->sink_input);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    memblockq_stream_unref(u);
}

static void memblockq_stream_free(pa_object *o) {
    memblockq_stream *u = MEMBLOCKQ_STREAM(o);
    pa_assert(u);

    memblockq_stream_unlink(u);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    pa_xfree(u);
}

static int memblockq_stream_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    memblockq_stream *u = MEMBLOCKQ_STREAM(o);
    memblockq_stream_assert_ref(u);

    switch (code) {
        case MEMBLOCKQ_STREAM_MESSAGE_UNLINK:
            memblockq_stream_unlink(u);
            break;
    }

    return 0;
}

static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    memblockq_stream_unlink(MEMBLOCKQ_STREAM(i->userdata));
}

static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    memblockq_stream *u;

    pa_assert(i);
    pa_assert(chunk);
    u = MEMBLOCKQ_STREAM(i->userdata);
    memblockq_stream_assert_ref(u);

    if (!u->memblockq)
        return -1;

    if (pa_memblockq_peek(u->memblockq, chunk) < 0) {
        pa_memblockq_free(u->memblockq);
        u->memblockq = NULL;
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u), MEMBLOCKQ_STREAM_MESSAGE_UNLINK, NULL, 0, NULL, NULL);
        return -1;
    }

    return 0;
}

static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    memblockq_stream *u;

    pa_assert(i);
    pa_assert(length > 0);
    u = MEMBLOCKQ_STREAM(i->userdata);
    memblockq_stream_assert_ref(u);

    if (!u->memblockq)
        return;

    pa_memblockq_drop(u->memblockq, length);
}

pa_sink_input* pa_memblockq_sink_input_new(
        pa_sink *sink,
        const char *name,
        const pa_sample_spec *ss,
        const pa_channel_map *map,
        pa_memblockq *q,
        pa_cvolume *volume) {

    memblockq_stream *u = NULL;
    pa_sink_input_new_data data;

    pa_assert(sink);
    pa_assert(ss);

    /* We allow creating this stream with no q set, so that it can be
     * filled in later */

    if (q && pa_memblockq_get_length(q) <= 0) {
        pa_memblockq_free(q);
        return NULL;
    }

    if (volume && pa_cvolume_is_muted(volume)) {
        pa_memblockq_free(q);
        return NULL;
    }

    u = pa_msgobject_new(memblockq_stream);
    u->parent.parent.free = memblockq_stream_free;
    u->parent.process_msg = memblockq_stream_process_msg;
    u->core = sink->core;
    u->sink_input = NULL;
    u->memblockq = q;

    pa_sink_input_new_data_init(&data);
    data.sink = sink;
    data.name = name;
    data.driver = __FILE__;
    pa_sink_input_new_data_set_sample_spec(&data, ss);
    pa_sink_input_new_data_set_channel_map(&data, map);
    pa_sink_input_new_data_set_volume(&data, volume);

    if (!(u->sink_input = pa_sink_input_new(sink->core, &data, 0)))
        goto fail;

    u->sink_input->peek = sink_input_peek_cb;
    u->sink_input->drop = sink_input_drop_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->userdata = u;

    if (q)
        pa_memblockq_prebuf_disable(q);

    /* The reference to u is dangling here, because we want
     * to keep this stream around until it is fully played. */

    /* This sink input is not "put" yet, i.e. pa_sink_input_put() has
     * not been called! */

    return pa_sink_input_ref(u->sink_input);

fail:
    if (u)
        memblockq_stream_unref(u);

    return NULL;
}

int pa_play_memblockq(
        pa_sink *sink,
        const char *name,
        const pa_sample_spec *ss,
        const pa_channel_map *map,
        pa_memblockq *q,
        pa_cvolume *volume) {

    pa_sink_input *i;

    pa_assert(sink);
    pa_assert(ss);
    pa_assert(q);

    if (!(i = pa_memblockq_sink_input_new(sink, name, ss, map, q, volume)))
        return -1;

    pa_sink_input_put(i);
    pa_sink_input_unref(i);

    return 0;
}

void pa_memblockq_sink_input_set_queue(pa_sink_input *i, pa_memblockq *q) {
    memblockq_stream *u;

    pa_sink_input_assert_ref(i);
    u = MEMBLOCKQ_STREAM(i->userdata);
    memblockq_stream_assert_ref(u);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);
    u->memblockq = q;
}
