#ifndef foopulsesourcehfoo
#define foopulsesourcehfoo

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
#include <pulsecore/asyncmsgq.h>
#include <pulsecore/msgobject.h>
#include <pulsecore/rtpoll.h>

#define PA_MAX_OUTPUTS_PER_SOURCE 32

typedef enum pa_source_state {
    PA_SOURCE_INIT,
    PA_SOURCE_RUNNING,
    PA_SOURCE_SUSPENDED,
    PA_SOURCE_IDLE,
    PA_SOURCE_UNLINKED
} pa_source_state_t;

static inline pa_bool_t PA_SOURCE_OPENED(pa_source_state_t x) {
    return x == PA_SOURCE_RUNNING || x == PA_SOURCE_IDLE;
}

static inline pa_bool_t PA_SOURCE_LINKED(pa_source_state_t x) {
    return x == PA_SOURCE_RUNNING || x == PA_SOURCE_IDLE || x == PA_SOURCE_SUSPENDED;
}

struct pa_source {
    pa_msgobject parent;

    uint32_t index;
    pa_core *core;
    pa_source_state_t state;
    pa_source_flags_t flags;

    char *name;
    char *description, *driver;              /* may be NULL */

    pa_module *module;                        /* may be NULL */

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *outputs;
    unsigned n_corked;
    pa_sink *monitor_of;                     /* may be NULL */

    pa_cvolume volume;
    pa_bool_t muted;
    pa_bool_t refresh_volume;
    pa_bool_t refresh_muted;

    int (*set_state)(pa_source*source, pa_source_state_t state); /* may be NULL */
    int (*set_volume)(pa_source *s);         /* dito */
    int (*get_volume)(pa_source *s);         /* dito */
    int (*set_mute)(pa_source *s);           /* dito */
    int (*get_mute)(pa_source *s);           /* dito */
    pa_usec_t (*get_latency)(pa_source *s);  /* dito */

    pa_asyncmsgq *asyncmsgq;
    pa_rtpoll *rtpoll;

    /* Contains copies of the above data so that the real-time worker
     * thread can work without access locking */
    struct {
        pa_source_state_t state;
        pa_hashmap *outputs;
        pa_cvolume soft_volume;
        pa_bool_t soft_muted;
    } thread_info;

    void *userdata;
};

PA_DECLARE_CLASS(pa_source);
#define PA_SOURCE(s) pa_source_cast(s)

typedef enum pa_source_message {
    PA_SOURCE_MESSAGE_ADD_OUTPUT,
    PA_SOURCE_MESSAGE_REMOVE_OUTPUT,
    PA_SOURCE_MESSAGE_GET_VOLUME,
    PA_SOURCE_MESSAGE_SET_VOLUME,
    PA_SOURCE_MESSAGE_GET_MUTE,
    PA_SOURCE_MESSAGE_SET_MUTE,
    PA_SOURCE_MESSAGE_GET_LATENCY,
    PA_SOURCE_MESSAGE_SET_STATE,
    PA_SOURCE_MESSAGE_PING,
    PA_SOURCE_MESSAGE_ATTACH,
    PA_SOURCE_MESSAGE_DETACH,
    PA_SOURCE_MESSAGE_MAX
} pa_source_message_t;

/* To be called exclusively by the source driver, from main context */

pa_source* pa_source_new(
        pa_core *core,
        const char *driver,
        const char *name,
        int namereg_fail,
        const pa_sample_spec *spec,
        const pa_channel_map *map);

void pa_source_put(pa_source *s);
void pa_source_unlink(pa_source *s);

void pa_source_set_module(pa_source *s, pa_module *m);
void pa_source_set_description(pa_source *s, const char *description);
void pa_source_set_asyncmsgq(pa_source *s, pa_asyncmsgq *q);
void pa_source_set_rtpoll(pa_source *s, pa_rtpoll *p);

void pa_source_detach(pa_source *s);
void pa_source_attach(pa_source *s);

/* May be called by everyone, from main context */

pa_usec_t pa_source_get_latency(pa_source *s);

int pa_source_update_status(pa_source*s);
int pa_source_suspend(pa_source *s, pa_bool_t suspend);
int pa_source_suspend_all(pa_core *c, pa_bool_t suspend);

void pa_source_ping(pa_source *s);

void pa_source_set_volume(pa_source *source, const pa_cvolume *volume);
const pa_cvolume *pa_source_get_volume(pa_source *source);
void pa_source_set_mute(pa_source *source, pa_bool_t mute);
pa_bool_t pa_source_get_mute(pa_source *source);

unsigned pa_source_linked_by(pa_source *s); /* Number of connected streams */
unsigned pa_source_used_by(pa_source *s); /* Number of connected streams that are not corked */
#define pa_source_get_state(s) ((pa_source_state_t) (s)->state)

/* To be called exclusively by the source driver, from IO context */

void pa_source_post(pa_source*s, const pa_memchunk *b);

int pa_source_process_msg(pa_msgobject *o, int code, void *userdata, int64_t, pa_memchunk *chunk);

void pa_source_attach_within_thread(pa_source *s);
void pa_source_detach_within_thread(pa_source *s);

#endif
