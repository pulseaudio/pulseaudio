#ifndef foosinkhfoo
#define foosinkhfoo

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

#include <inttypes.h>

typedef struct pa_sink pa_sink;

#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "source.h"
#include "typeid.h"
#include "module.h"

#define PA_MAX_INPUTS_PER_SINK 6

typedef enum pa_sink_state {
    PA_SINK_RUNNING,
    PA_SINK_DISCONNECTED
} pa_sink_state;

struct pa_sink {
    int ref;
    pa_sink_state state;
    
    uint32_t index;
    pa_typeid_t typeid;

    char *name, *description;
    pa_module *owner;
    pa_core *core;
    pa_sample_spec sample_spec;
    pa_idxset *inputs;

    pa_source *monitor_source;

    pa_volume_t volume;

    void (*notify)(pa_sink*sink);
    pa_usec_t (*get_latency)(pa_sink *s);
    void *userdata;
};

pa_sink* pa_sink_new(pa_core *core, pa_typeid_t typeid, const char *name, int fail, const pa_sample_spec *spec);
void pa_sink_disconnect(pa_sink* s);
void pa_sink_unref(pa_sink*s);
pa_sink* pa_sink_ref(pa_sink *s);

int pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result);
void pa_sink_render_full(pa_sink *s, size_t length, pa_memchunk *result);
int pa_sink_render_into(pa_sink*s, pa_memchunk *target);
void pa_sink_render_into_full(pa_sink *s, pa_memchunk *target);
    
pa_usec_t pa_sink_get_latency(pa_sink *s);

void pa_sink_notify(pa_sink*s);

void pa_sink_set_owner(pa_sink *sink, pa_module *m);

void pa_sink_set_volume(pa_sink *sink, pa_volume_t volume);

#endif
