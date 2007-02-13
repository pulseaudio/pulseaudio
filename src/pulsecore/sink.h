#ifndef foosinkhfoo
#define foosinkhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <inttypes.h>

typedef struct pa_sink pa_sink;

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>
#include <pulsecore/core-def.h>
#include <pulsecore/core.h>
#include <pulsecore/idxset.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>

#define PA_MAX_INPUTS_PER_SINK 32

typedef enum pa_sink_state {
    PA_SINK_RUNNING,
    PA_SINK_DISCONNECTED
} pa_sink_state_t;

struct pa_sink {
    int ref;
    uint32_t index;
    pa_core *core;
    pa_sink_state_t state;

    char *name;
    char *description, *driver;            /* may be NULL */
    int is_hardware;

    pa_module *owner;                      /* may be NULL */

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *inputs;
    pa_source *monitor_source;             /* may be NULL */

    pa_cvolume hw_volume, sw_volume;
    int hw_muted, sw_muted;

    void (*notify)(pa_sink*sink);          /* may be NULL */
    pa_usec_t (*get_latency)(pa_sink *s);  /* dito */
    int (*set_hw_volume)(pa_sink *s);      /* dito */
    int (*get_hw_volume)(pa_sink *s);      /* dito */
    int (*set_hw_mute)(pa_sink *s);        /* dito */
    int (*get_hw_mute)(pa_sink *s);        /* dito */

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
void pa_sink_set_mute(pa_sink *sink, pa_mixer_t m, int mute);
int pa_sink_get_mute(pa_sink *sink, pa_mixer_t m);

void pa_sink_set_description(pa_sink *s, const char *description);

unsigned pa_sink_used_by(pa_sink *s);

#endif
