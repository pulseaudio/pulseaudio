/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "sink-input.h"
#include "module.h"
#include "modargs.h"
#include "xmalloc.h"
#include "namereg.h"
#include "log.h"
#include "module-sine-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Sine wave generator")
PA_MODULE_USAGE("sink=<sink to connect to> frequency=<frequency in Hz>")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define PA_TYPEID_SINE PA_TYPEID_MAKE('S', 'I', 'N', 'E')

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink_input *sink_input;
    pa_memblock *memblock;
    size_t peek_index;
};

static const char* const valid_modargs[] = {
    "sink",
    "frequency",
    NULL,
};

static int sink_input_peek(pa_sink_input *i, pa_memchunk *chunk) {
    struct userdata *u;
    assert(i && chunk && i->userdata);
    u = i->userdata;

    chunk->memblock = pa_memblock_ref(u->memblock);
    chunk->index = u->peek_index;
    chunk->length = u->memblock->length - u->peek_index;
    return 0;
}

static void sink_input_drop(pa_sink_input *i, const pa_memchunk *chunk, size_t length) {
    struct userdata *u;
    assert(i && chunk && length && i->userdata);
    u = i->userdata;

    assert(chunk->memblock == u->memblock && length <= u->memblock->length-u->peek_index);

    u->peek_index += length;

    if (u->peek_index >= u->memblock->length)
        u->peek_index = 0;
}

static void sink_input_kill(pa_sink_input *i) {
    struct userdata *u;
    assert(i && i->userdata);
    u = i->userdata;

    pa_sink_input_disconnect(u->sink_input);
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_module_unload_request(u->module);
}

static void calc_sine(float *f, size_t l, float freq) {
    size_t i;

    l /= sizeof(float);
    
    for (i = 0; i < l; i++)
        f[i] = (float) sin((double) i/l*M_PI*2*freq)/2;
}

int pa__init(pa_core *c, pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    pa_sink *sink;
    const char *sink_name;
    pa_sample_spec ss;
    uint32_t frequency;
    char t[256];

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": Failed to parse module arguments\n");
        goto fail;
    }
    
    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->module = m;
    u->sink_input = NULL;
    u->memblock = NULL;

    sink_name = pa_modargs_get_value(ma, "sink", NULL);

    if (!(sink = pa_namereg_get(c, sink_name, PA_NAMEREG_SINK, 1))) {
        pa_log(__FILE__": No such sink.\n");
        goto fail;
    }

    ss.format = PA_SAMPLE_FLOAT32;
    ss.rate = sink->sample_spec.rate;
    ss.channels = 1;

    frequency = 440;
    if (pa_modargs_get_value_u32(ma, "frequency", &frequency) < 0 || frequency < 1 || frequency > ss.rate/2) {
        pa_log(__FILE__": Invalid frequency specification\n");
        goto fail;
    }
    
    u->memblock = pa_memblock_new(pa_bytes_per_second(&ss), c->memblock_stat);
    calc_sine(u->memblock->data, u->memblock->length, frequency);

    snprintf(t, sizeof(t), "Sine Generator at %u Hz", frequency);
    if (!(u->sink_input = pa_sink_input_new(sink, PA_TYPEID_SINE, t, &ss, 0, -1)))
        goto fail;

    u->sink_input->peek = sink_input_peek;
    u->sink_input->drop = sink_input_drop;
    u->sink_input->kill = sink_input_kill;
    u->sink_input->userdata = u;
    u->sink_input->owner = m;

    u->peek_index = 0;
    
    pa_modargs_free(ma);
    return 0;
    
fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(c, m);
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u = m->userdata;
    assert(c && m);

    if (!u)
        return;

    if (u->sink_input) {
        pa_sink_input_disconnect(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }
    
    if (u->memblock)
        pa_memblock_unref(u->memblock);
    pa_xfree(u);
}

