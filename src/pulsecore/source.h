#ifndef foosourcehfoo
#define foosourcehfoo

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

typedef struct pa_source pa_source;

#include <inttypes.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>
#include <pulsecore/core-def.h>
#include <pulsecore/core.h>
#include <pulsecore/idxset.h>
#include <pulsecore/memblock.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>

#define PA_MAX_OUTPUTS_PER_SOURCE 16

typedef enum pa_source_state {
    PA_SOURCE_RUNNING,
    PA_SOURCE_DISCONNECTED
} pa_source_state_t;

struct pa_source {
    int ref;
    uint32_t index;
    pa_core *core;
    pa_source_state_t state;
    
    char *name;
    char *description, *driver;              /* may be NULL */
    
    pa_module *owner;                        /* may be NULL */

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *outputs;
    pa_sink *monitor_of;                     /* may be NULL */

    pa_cvolume hw_volume, sw_volume;
    int hw_muted, sw_muted;

    int is_hardware;
    
    void (*notify)(pa_source*source);        /* may be NULL */
    pa_usec_t (*get_latency)(pa_source *s);  /* dito */
    int (*set_hw_volume)(pa_source *s);      /* dito */
    int (*get_hw_volume)(pa_source *s);      /* dito */ 
    int (*set_hw_mute)(pa_source *s);        /* dito */
    int (*get_hw_mute)(pa_source *s);        /* dito */
    
    void *userdata;
};

pa_source* pa_source_new(
    pa_core *core,
    const char *driver,
    const char *name,
    int namereg_fail,
    const pa_sample_spec *spec,
    const pa_channel_map *map);

void pa_source_disconnect(pa_source *s);
void pa_source_unref(pa_source *s);
pa_source* pa_source_ref(pa_source *c);

/* Pass a new memory block to all output streams */
void pa_source_post(pa_source*s, const pa_memchunk *b);

void pa_source_notify(pa_source *s);

void pa_source_set_owner(pa_source *s, pa_module *m);

pa_usec_t pa_source_get_latency(pa_source *s);

void pa_source_set_volume(pa_source *source, pa_mixer_t m, const pa_cvolume *volume);
const pa_cvolume *pa_source_get_volume(pa_source *source, pa_mixer_t m);
void pa_source_set_mute(pa_source *source, pa_mixer_t m, int mute);
int pa_source_get_mute(pa_source *source, pa_mixer_t m);

void pa_source_set_description(pa_source *s, const char *description);

unsigned pa_source_used_by(pa_source *s);
#endif
