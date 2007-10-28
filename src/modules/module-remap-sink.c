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

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

#include "module-remap-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Virtual channel remapping sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "master=<name of sink to remap> "
        "master_channel_map=<channel map> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>")

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_sink *sink, *master;
    pa_sink_input *sink_input;

    pa_memchunk memchunk;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "master",
    "master_channel_map",
    "rate",
    "format",
    "channels",
    "channel_map",
    NULL
};

/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;

            if (PA_MSGOBJECT(u->master)->process_msg(PA_MSGOBJECT(u->master), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            *((pa_usec_t*) data) = usec + pa_bytes_to_usec(u->memchunk.length, &u->sink->sample_spec);
            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (PA_SINK_LINKED(state) && u->sink_input && PA_SINK_INPUT_LINKED(pa_sink_input_get_state(u->sink_input)))
        pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);

    return 0;
}

/* Called from I/O thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK_INPUT(o)->userdata;

    switch (code) {
        case PA_SINK_INPUT_MESSAGE_GET_LATENCY:
            *((pa_usec_t*) data) = pa_bytes_to_usec(u->memchunk.length, &u->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
    }

    return pa_sink_input_process_msg(o, code, data, offset, chunk);
}

/* Called from I/O thread context */
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->memchunk.memblock)
        pa_sink_render(u->sink, length, &u->memchunk);

    pa_assert(u->memchunk.memblock);
    *chunk = u->memchunk;
    pa_memblock_ref(chunk->memblock);
    return 0;
}

/* Called from I/O thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(length > 0);

    if (u->memchunk.memblock) {

        if (length < u->memchunk.length) {
            u->memchunk.index += length;
            u->memchunk.length -= length;
            return;
        }

        pa_memblock_unref(u->memchunk.memblock);
        length -= u->memchunk.length;
        pa_memchunk_reset(&u->memchunk);
    }

    if (length > 0)
        pa_sink_skip(u->sink, length);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_detach_within_thread(u->sink);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_asyncmsgq(u->sink, i->sink->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, i->sink->rtpoll);

    pa_sink_attach_within_thread(u->sink);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_input_unlink(u->sink_input);
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unlink(u->sink);
    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_module_unload_request(u->module);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map sink_map, stream_map;
    pa_modargs *ma;
    char *t;
    pa_sink *master;
    pa_sink_input_new_data data;
    char *default_sink_name = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "master", NULL), PA_NAMEREG_SINK, 1))) {
        pa_log("Master sink not found");
        goto fail;
    }

    ss = master->sample_spec;
    sink_map = master->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &sink_map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    stream_map = sink_map;
    if (pa_modargs_get_channel_map(ma, "master_channel_map", &stream_map) < 0) {
        pa_log("Invalid master hannel map");
        goto fail;
    }

    if (stream_map.channels != ss.channels) {
        pa_log("Number of channels doesn't match");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->master = master;
    pa_memchunk_reset(&u->memchunk);

    default_sink_name = pa_sprintf_malloc("%s.remapped", master->name);

    /* Create sink */
    if (!(u->sink = pa_sink_new(m->core, __FILE__, pa_modargs_get_value(ma, "sink_name", default_sink_name), 0, &ss, &sink_map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state = sink_set_state;
    u->sink->userdata = u;
    u->sink->flags = PA_SINK_LATENCY;

    pa_sink_set_module(u->sink, m);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Remapped %s", master->description));
    pa_xfree(t);
    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, master->rtpoll);

    /* Create sink input */
    pa_sink_input_new_data_init(&data);
    data.sink = u->master;
    data.driver = __FILE__;
    data.name = "Remapped Stream";
    pa_sink_input_new_data_set_sample_spec(&data, &ss);
    pa_sink_input_new_data_set_channel_map(&data, &stream_map);
    data.module = m;

    if (!(u->sink_input = pa_sink_input_new(m->core, &data, PA_SINK_INPUT_DONT_MOVE)))
        goto fail;

    u->sink_input->parent.process_msg = sink_input_process_msg;
    u->sink_input->peek = sink_input_peek_cb;
    u->sink_input->drop = sink_input_drop_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->userdata = u;

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);
    pa_xfree(default_sink_name);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    pa_xfree(default_sink_name);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }

    if (u->sink) {
        pa_sink_unlink(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    pa_xfree(u);
}
