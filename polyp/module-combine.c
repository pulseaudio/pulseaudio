/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>

#include "module.h"
#include "llist.h"
#include "sink.h"
#include "sink-input.h"
#include "memblockq.h"
#include "log.h"
#include "util.h"
#include "xmalloc.h"
#include "modargs.h"
#include "namereg.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Combine multiple sinks to one")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("sink_name=<name for the sink> master=<master sink> slave=<slave sinks>")

#define DEFAULT_SINK_NAME "combine"
#define MEMBLOCKQ_MAXLENGTH (1024*170)
#define RENDER_SIZE (1024*10)

#define ADJUST_TIME 5

static const char* const valid_modargs[] = {
    "sink_name",
    "master",
    "slaves",
    NULL
};

struct output {
    struct userdata *userdata;
    struct pa_sink_input *sink_input;
    size_t counter;
    struct pa_memblockq *memblockq;
    pa_usec_t sink_latency;
    PA_LLIST_FIELDS(struct output);
};

struct userdata {
    struct pa_module *module;
    struct pa_core *core;
    struct pa_sink *sink;
    unsigned n_outputs;
    struct output *master;
    struct pa_time_event *time_event;
    
    PA_LLIST_HEAD(struct output, outputs);
};

static void output_free(struct output *o);
static void clear_up(struct userdata *u);

static void adjust_rates(struct userdata *u) {
    struct output *o;
    pa_usec_t max  = 0;
    uint32_t base_rate;
    assert(u && u->sink);

    for (o = u->outputs; o; o = o->next) {
        o->sink_latency = o->sink_input->sink ? pa_sink_get_latency(o->sink_input->sink) : 0;

        if (o->sink_latency > max)
            max = o->sink_latency;
    }

    pa_log(__FILE__": [%s] maximum latency is %0.0f usec.\n", u->sink->name, (float) max);

    base_rate = u->sink->sample_spec.rate;

    for (o = u->outputs; o; o = o->next) {
        pa_usec_t l;
        uint32_t r = base_rate;

        l = o->sink_latency + pa_sink_input_get_latency(o->sink_input);

        if (l < max)
            r -= (uint32_t) (((((double) max-l))/ADJUST_TIME)*r/ 1000000);
        else if (l > max)
            r += (uint32_t) (((((double) l-max))/ADJUST_TIME)*r/ 1000000);

        if (r < (uint32_t) (base_rate*0.9) || r > (uint32_t) (base_rate*1.1))
            pa_log(__FILE__": [%s] sample rates too different, not adjusting (%u vs. %u).\n", o->sink_input->name, base_rate, r);
        else
            pa_log(__FILE__": [%s] new rate is %u Hz; ratio is %0.3f; latency is %0.0f usec.\n", o->sink_input->name, r, (double) r / base_rate, (float) l);
        
        pa_sink_input_set_rate(o->sink_input, r);
    }
}

static void request_memblock(struct userdata *u) {
    struct pa_memchunk chunk;
    struct output *o;
    assert(u && u->sink);

    if (pa_sink_render(u->sink, RENDER_SIZE, &chunk) < 0)
        return;

    for (o = u->outputs; o; o = o->next)
        pa_memblockq_push_align(o->memblockq, &chunk, 0);

    pa_memblock_unref(chunk.memblock);
}

static void time_callback(struct pa_mainloop_api*a, struct pa_time_event* e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval n;
    assert(u && a && u->time_event == e);

    adjust_rates(u);

    gettimeofday(&n, NULL);
    n.tv_sec += ADJUST_TIME;
    u->sink->core->mainloop->time_restart(e, &n);
}

static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    struct output *o = i->userdata;
    assert(i && o && o->sink_input && chunk);

    if (pa_memblockq_peek(o->memblockq, chunk) >= 0)
        return 0;
    
    /* Try harder */
    request_memblock(o->userdata);
    
    return pa_memblockq_peek(o->memblockq, chunk);
}

static void sink_input_drop_cb(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length) {
    struct output *o = i->userdata;
    assert(i && o && o->sink_input && chunk && length);

    pa_memblockq_drop(o->memblockq, chunk, length);
    o->counter += length;
}

static void sink_input_kill_cb(struct pa_sink_input *i) {
    struct output *o = i->userdata;
    assert(i && o && o->sink_input);
    pa_module_unload_request(o->userdata->module);
    clear_up(o->userdata);
}

static pa_usec_t sink_input_get_latency_cb(struct pa_sink_input *i) {
    struct output *o = i->userdata;
    assert(i && o && o->sink_input);
    
    return pa_bytes_to_usec(pa_memblockq_get_length(o->memblockq), &i->sample_spec);
}

static pa_usec_t sink_get_latency_cb(struct pa_sink *s) {
    struct userdata *u = s->userdata;
    assert(s && u && u->sink && u->master);

    return pa_sink_input_get_latency(u->master->sink_input);
}

static struct output *output_new(struct userdata *u, struct pa_sink *sink) {
    struct output *o = NULL;
    char t[256];
    assert(u && sink && u->sink);
    
    o = pa_xmalloc(sizeof(struct output));
    o->userdata = u;
    
    o->counter = 0;
    o->memblockq = pa_memblockq_new(MEMBLOCKQ_MAXLENGTH, MEMBLOCKQ_MAXLENGTH, pa_frame_size(&u->sink->sample_spec), 0, 0, sink->core->memblock_stat);

    snprintf(t, sizeof(t), "%s: output #%u", u->sink->name, u->n_outputs+1);
    if (!(o->sink_input = pa_sink_input_new(sink, t, &u->sink->sample_spec, 1)))
        goto fail;

    o->sink_input->get_latency = sink_input_get_latency_cb;
    o->sink_input->peek = sink_input_peek_cb;
    o->sink_input->drop = sink_input_drop_cb;
    o->sink_input->kill = sink_input_kill_cb;
    o->sink_input->userdata = o;
    o->sink_input->owner = u->module;
    
    PA_LLIST_PREPEND(struct output, u->outputs, o);
    u->n_outputs++;
    return o;

fail:

    if (o) {
        if (o->sink_input) {
            pa_sink_input_disconnect(o->sink_input);
            pa_sink_input_unref(o->sink_input);
        }

        if (o->memblockq)
            pa_memblockq_free(o->memblockq);
        
        pa_xfree(o);
    }

    return NULL;
}

static void output_free(struct output *o) {
    assert(o);
    PA_LLIST_REMOVE(struct output, o->userdata->outputs, o);
    o->userdata->n_outputs--;
    pa_memblockq_free(o->memblockq);
    pa_sink_input_disconnect(o->sink_input);
    pa_sink_input_unref(o->sink_input);
    pa_xfree(o);
}

static void clear_up(struct userdata *u) {
    struct output *o;
    assert(u);
    
    if (u->time_event) {
        u->core->mainloop->time_free(u->time_event);
        u->time_event = NULL;
    }
    
    while ((o = u->outputs))
        output_free(o);

    u->master = NULL;
    
    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    struct pa_modargs *ma = NULL;
    const char *master_name, *slaves;
    struct pa_sink *master_sink;
    char *n = NULL;
    const char*split_state;
    struct timeval tv;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments\n");
        goto fail;
    }
    
    u = pa_xmalloc(sizeof(struct userdata));
    m->userdata = u;
    u->sink = NULL;
    u->n_outputs = 0;
    u->master = NULL;
    u->module = m;
    u->core = c;
    u->time_event = NULL;
    PA_LLIST_HEAD_INIT(struct output, u->outputs);

    if (!(master_name = pa_modargs_get_value(ma, "master", NULL)) || !(slaves = pa_modargs_get_value(ma, "slaves", NULL))) {
        pa_log(__FILE__": no master or slave sinks specified\n");
        goto fail;
    }

    if (!(master_sink = pa_namereg_get(c, master_name, PA_NAMEREG_SINK, 1))) {
        pa_log(__FILE__": invalid master sink '%s'\n", master_name);
        goto fail;
    }

    if (!(u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &master_sink->sample_spec))) {
        pa_log(__FILE__": failed to create sink\n");
        goto fail;
    }

    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("Combined sink");
    u->sink->get_latency = sink_get_latency_cb;
    u->sink->userdata = u;
    
    if (!(u->master = output_new(u, master_sink))) {
        pa_log(__FILE__": failed to create master sink input on sink '%s'.\n", u->sink->name);
        goto fail;
    }
    
    split_state = NULL;
    while ((n = pa_split(slaves, ",", &split_state))) {
        struct pa_sink *slave_sink;
        
        if (!(slave_sink = pa_namereg_get(c, n, PA_NAMEREG_SINK, 1))) {
            pa_log(__FILE__": invalid slave sink '%s'\n", n);
            goto fail;
        }

        pa_xfree(n);

        if (!output_new(u, slave_sink)) {
            pa_log(__FILE__": failed to create slave sink input on sink '%s'.\n", slave_sink->name);
            goto fail;
        }
    }
           
    if (u->n_outputs <= 1)
        pa_log(__FILE__": WARNING: no slave sinks specified.\n");

    gettimeofday(&tv, NULL);
    tv.tv_sec += ADJUST_TIME;
    u->time_event = c->mainloop->time_new(c->mainloop, &tv, time_callback, u);
    
    pa_modargs_free(ma);
    return 0;    

fail:
    pa_xfree(n);
    
    if (ma)
        pa_modargs_free(ma);

    pa__done(c, m);
    return -1;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    clear_up(u);
    pa_xfree(u);
}


