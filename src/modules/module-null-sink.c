/* $Id$ */

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

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>

#include "module-null-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Clocked NULL sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "sink_name=<name of sink>"
        "channel_map=<channel map>")

#define DEFAULT_SINK_NAME "null"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_time_event *time_event;
    size_t block_size;

    uint64_t n_bytes;
    struct timeval start_time;
};

static const char* const valid_modargs[] = {
    "rate",
    "format",
    "channels",
    "sink_name",
    "channel_map", 
    NULL
};

static void time_callback(pa_mainloop_api *m, pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    pa_memchunk chunk;
    struct timeval ntv = *tv;
    size_t l;

    assert(u);

    if (pa_sink_render(u->sink, u->block_size, &chunk) >= 0) {
        l = chunk.length;
        pa_memblock_unref(chunk.memblock);
    } else
        l = u->block_size;

    pa_timeval_add(&ntv, pa_bytes_to_usec(l, &u->sink->sample_spec));
    m->time_restart(e, &ntv);

    u->n_bytes += l;
}

static pa_usec_t get_latency(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_usec_t a, b;
    struct timeval now;

    a = pa_timeval_diff(pa_gettimeofday(&now), &u->start_time);
    b = pa_bytes_to_usec(u->n_bytes, &s->sample_spec);

    return b > a ? b - a : 0;
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    
    assert(c);
    assert(m);
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log(__FILE__": invalid sample format specification or channel map.");
        goto fail;
    }
    
    u = pa_xnew0(struct userdata, 1);
    u->core = c;
    u->module = m;
    m->userdata = u;
    
    if (!(u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log(__FILE__": failed to create sink.");
        goto fail;
    }

    u->sink->get_latency = get_latency;
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_xstrdup("NULL sink");

    u->n_bytes = 0;
    pa_gettimeofday(&u->start_time);
    
    u->time_event = c->mainloop->time_new(c->mainloop, &u->start_time, time_callback, u);

    u->block_size = pa_bytes_per_second(&ss) / 10;
    
    pa_modargs_free(ma);
    
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
        
    pa__done(c, m);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    pa_sink_disconnect(u->sink);
    pa_sink_unref(u->sink);

    u->core->mainloop->time_free(u->time_event);

    pa_xfree(u);
}
