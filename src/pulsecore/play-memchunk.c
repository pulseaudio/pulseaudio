/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include "play-memchunk.h"

typedef struct memchunk_stream {
    pa_msgobject parent;
    pa_core *core;
    pa_sink_input *sink_input;
    pa_memchunk memchunk;
} memchunk_stream;

enum {
    MEMCHUNK_STREAM_MESSAGE_UNLINK,
};

PA_DECLARE_CLASS(memchunk_stream);
#define MEMCHUNK_STREAM(o) (memchunk_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(memchunk_stream, pa_msgobject);

static void memchunk_stream_unlink(memchunk_stream *u) {
    pa_assert(u);

    if (!u->sink_input)
        return;

    pa_sink_input_unlink(u->sink_input);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    memchunk_stream_unref(u);
}

static void memchunk_stream_free(pa_object *o) {
    memchunk_stream *u = MEMCHUNK_STREAM(o);
    pa_assert(u);

    memchunk_stream_unlink(u);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    pa_xfree(u);
}

static int memchunk_stream_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    memchunk_stream *u = MEMCHUNK_STREAM(o);
    memchunk_stream_assert_ref(u);

    switch (code) {
        case MEMCHUNK_STREAM_MESSAGE_UNLINK:
            memchunk_stream_unlink(u);
            break;
    }

    return 0;
}

static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    memchunk_stream_unlink(MEMCHUNK_STREAM(i->userdata));
}

static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    memchunk_stream *u;

    pa_assert(i);
    pa_assert(chunk);
    u = MEMCHUNK_STREAM(i->userdata);
    memchunk_stream_assert_ref(u);

    if (!u->memchunk.memblock)
        return -1;

    if (u->memchunk.length <= 0) {
        pa_memblock_unref(u->memchunk.memblock);
        u->memchunk.memblock = NULL;
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u), MEMCHUNK_STREAM_MESSAGE_UNLINK, NULL, 0, NULL, NULL);
        return -1;
    }

    pa_assert(u->memchunk.memblock);
    *chunk = u->memchunk;
    pa_memblock_ref(chunk->memblock);

    return 0;
}

static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    memchunk_stream *u;

    pa_assert(i);
    pa_assert(length > 0);
    u = MEMCHUNK_STREAM(i->userdata);
    memchunk_stream_assert_ref(u);

    if (length < u->memchunk.length) {
        u->memchunk.length -= length;
        u->memchunk.index += length;
    } else
        u->memchunk.length = 0;
}

int pa_play_memchunk(
        pa_sink *sink,
        const char *name,
        const pa_sample_spec *ss,
        const pa_channel_map *map,
        const pa_memchunk *chunk,
        pa_cvolume *volume) {

    memchunk_stream *u = NULL;
    pa_sink_input_new_data data;

    pa_assert(sink);
    pa_assert(ss);
    pa_assert(chunk);

    if (volume && pa_cvolume_is_muted(volume))
        return 0;

    pa_memchunk_will_need(chunk);

    u = pa_msgobject_new(memchunk_stream);
    u->parent.parent.free = memchunk_stream_free;
    u->parent.process_msg = memchunk_stream_process_msg;
    u->core = sink->core;
    u->memchunk = *chunk;
    pa_memblock_ref(u->memchunk.memblock);

    pa_sink_input_new_data_init(&data);
    data.sink = sink;
    data.driver = __FILE__;
    data.name = name;
    pa_sink_input_new_data_set_sample_spec(&data, ss);
    pa_sink_input_new_data_set_channel_map(&data, map);
    pa_sink_input_new_data_set_volume(&data, volume);

    if (!(u->sink_input = pa_sink_input_new(sink->core, &data, 0)))
        goto fail;

    u->sink_input->peek = sink_input_peek_cb;
    u->sink_input->drop = sink_input_drop_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->userdata = u;

    pa_sink_input_put(u->sink_input);

    /* The reference to u is dangling here, because we want to keep
     * this stream around until it is fully played. */

    return 0;

fail:
    if (u)
        memchunk_stream_unref(u);

    return -1;
}

