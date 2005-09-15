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

struct pa_sink;

#include <inttypes.h>

#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "source.h"
#include "channelmap.h"

#define PA_MAX_INPUTS_PER_SINK 6

typedef enum {
    PA_SINK_RUNNING,
    PA_SINK_DISCONNECTED
} pa_sink_state_t;

typedef enum {
    PA_MIXER_AUTO,
    PA_MIXER_SOFTWARE,
    PA_MIXER_HARDWARE
} pa_mixer_t;

struct pa_sink {
    int ref;
    uint32_t index;
    struct pa_core *core;
    pa_sink_state_t state;

    char *name, *description, *driver;
    struct pa_sample_spec sample_spec;
    struct pa_channel_map channel_map;
    struct pa_idxset *inputs;
    struct pa_module *owner;
    struct pa_source *monitor_source;
    struct pa_cvolume hw_volume, sw_volume;

    void (*notify)(struct pa_sink*sink);
    pa_usec_t (*get_latency)(struct pa_sink *s);
    void (*set_volume)(struct pa_sink *s);
    void (*get_volume)(struct pa_sink *s);
    
    void *userdata;
};

struct pa_sink* pa_sink_new(
    struct pa_core *core,
    const char *name,
    const char *driver,
    int fail,
    const struct pa_sample_spec *spec,
    const struct pa_channel_map *map);

void pa_sink_disconnect(struct pa_sink* s);
void pa_sink_unref(struct pa_sink*s);
struct pa_sink* pa_sink_ref(struct pa_sink *s);

int pa_sink_render(struct pa_sink*s, size_t length, struct pa_memchunk *result);
void pa_sink_render_full(struct pa_sink *s, size_t length, struct pa_memchunk *result);
int pa_sink_render_into(struct pa_sink*s, struct pa_memchunk *target);
void pa_sink_render_into_full(struct pa_sink *s, struct pa_memchunk *target);
    
pa_usec_t pa_sink_get_latency(struct pa_sink *s);

void pa_sink_notify(struct pa_sink*s);

void pa_sink_set_owner(struct pa_sink *sink, struct pa_module *m);

void pa_sink_set_volume(struct pa_sink *sink, pa_mixer_t m, const struct pa_cvolume *volume);
const struct pa_cvolume *pa_sink_get_volume(struct pa_sink *sink, pa_mixer_t m);

#endif
