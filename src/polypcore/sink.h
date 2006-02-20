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

#include <polyp/sample.h>
#include <polyp/channelmap.h>
#include <polyp/volume.h>
#include <polypcore/core.h>
#include <polypcore/idxset.h>
#include <polypcore/source.h>
#include <polypcore/module.h>

#define PA_MAX_INPUTS_PER_SINK 32

typedef enum pa_sink_state {
    PA_SINK_RUNNING,
    PA_SINK_DISCONNECTED
} pa_sink_state_t;

typedef enum pa_mixer {
    PA_MIXER_SOFTWARE,
    PA_MIXER_HARDWARE
} pa_mixer_t;

struct pa_sink {
    int ref;
    uint32_t index;
    pa_core *core;
    pa_sink_state_t state;

    char *name, *description, *driver;
    pa_module *owner;

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *inputs;
    pa_source *monitor_source;
    
    pa_cvolume hw_volume, sw_volume;

    void (*notify)(pa_sink*sink);
    pa_usec_t (*get_latency)(pa_sink *s);
    int (*set_hw_volume)(pa_sink *s);
    int (*get_hw_volume)(pa_sink *s);
    
    void *userdata;
};

pa_sink* pa_sink_new(
    pa_core *core,
    const char *driver,
    const char *name,
    int namereg_fail,
    const pa_sample_spec *spec,
    const pa_channel_map *map);

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

void pa_sink_set_volume(pa_sink *sink, pa_mixer_t m, const pa_cvolume *volume);
const pa_cvolume *pa_sink_get_volume(pa_sink *sink, pa_mixer_t m);

#endif
