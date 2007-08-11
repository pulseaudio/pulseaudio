#ifndef foopulsesinkhfoo
#define foopulsesinkhfoo

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

typedef struct pa_sink pa_sink;

#include <inttypes.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>

#include <pulsecore/core-def.h>
#include <pulsecore/core.h>
#include <pulsecore/idxset.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/msgobject.h>

#define PA_MAX_INPUTS_PER_SINK 32

typedef enum pa_sink_state {
    PA_SINK_RUNNING,
    PA_SINK_SUSPENDED,
    PA_SINK_IDLE,
    PA_SINK_DISCONNECTED
} pa_sink_state_t;

struct pa_sink {
    pa_msgobject parent;

    uint32_t index;
    pa_core *core;
    pa_sink_state_t state;

    char *name;
    char *description, *driver;            /* may be NULL */
    int is_hardware;

    pa_module *module;                      /* may be NULL */

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *inputs;
    pa_source *monitor_source;             /* may be NULL */

    pa_cvolume volume;
    int muted;
    int refresh_volume;
    int refresh_mute;

    int (*set_state)(pa_sink *s, pa_sink_state_t state);
    int (*set_volume)(pa_sink *s);      /* dito */
    int (*get_volume)(pa_sink *s);      /* dito */
    int (*get_mute)(pa_sink *s);        /* dito */
    int (*set_mute)(pa_sink *s);        /* dito */
    pa_usec_t (*get_latency)(pa_sink *s);    /* dito */

    pa_asyncmsgq *asyncmsgq;

    /* Contains copies of the above data so that the real-time worker
     * thread can work without access locking */
    struct {
        pa_sink_state_t state;
        pa_hashmap *inputs;
        pa_cvolume soft_volume;
        int soft_muted;
    } thread_info;

    pa_memblock *silence;

    void *userdata;
};

PA_DECLARE_CLASS(pa_sink);
#define PA_SINK(s) (pa_sink_cast(s))

typedef enum pa_sink_message {
    PA_SINK_MESSAGE_ADD_INPUT,
    PA_SINK_MESSAGE_REMOVE_INPUT,
    PA_SINK_MESSAGE_GET_VOLUME,
    PA_SINK_MESSAGE_SET_VOLUME,
    PA_SINK_MESSAGE_GET_MUTE,
    PA_SINK_MESSAGE_SET_MUTE,
    PA_SINK_MESSAGE_GET_LATENCY,
    PA_SINK_MESSAGE_SET_STATE,
    PA_SINK_MESSAGE_PING,
    PA_SINK_MESSAGE_REMOVE_INPUT_AND_BUFFER,
    PA_SINK_MESSAGE_MAX
} pa_sink_message_t;

/* To be used exclusively by the sink driver */

pa_sink* pa_sink_new(
        pa_core *core,
        const char *driver,
        const char *name,
        int namereg_fail,
        const pa_sample_spec *spec,
        const pa_channel_map *map);

void pa_sink_disconnect(pa_sink* s);

void pa_sink_set_module(pa_sink *sink, pa_module *m);
void pa_sink_set_description(pa_sink *s, const char *description);
void pa_sink_set_asyncmsgq(pa_sink *s, pa_asyncmsgq *q);

/* Usable by everyone */

pa_usec_t pa_sink_get_latency(pa_sink *s);

int pa_sink_update_status(pa_sink*s);
int pa_sink_suspend(pa_sink *s, int suspend);

/* Sends a ping message to the sink thread, to make it wake up and
 * check for data to process even if there is no real message is
 * sent */
void pa_sink_ping(pa_sink *s); 

void pa_sink_set_volume(pa_sink *sink, const pa_cvolume *volume);
const pa_cvolume *pa_sink_get_volume(pa_sink *sink);
void pa_sink_set_mute(pa_sink *sink, int mute);
int pa_sink_get_mute(pa_sink *sink);

unsigned pa_sink_used_by(pa_sink *s);
#define pa_sink_get_state(s) ((s)->state)

/* To be used exclusively by the sink driver thread */

void pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result);
void pa_sink_render_full(pa_sink *s, size_t length, pa_memchunk *result);
void pa_sink_render_into(pa_sink*s, pa_memchunk *target);
void pa_sink_render_into_full(pa_sink *s, pa_memchunk *target);

int pa_sink_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk);

static inline int PA_SINK_OPENED(pa_sink_state_t x) {
    return x == PA_SINK_RUNNING || x == PA_SINK_IDLE;
}

#endif
