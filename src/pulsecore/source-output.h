#ifndef foopulsesourceoutputhfoo
#define foopulsesourceoutputhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

typedef struct pa_source_output pa_source_output;

#include <pulse/sample.h>
#include <pulsecore/source.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/resampler.h>
#include <pulsecore/module.h>
#include <pulsecore/client.h>

typedef enum pa_source_output_state {
    PA_SOURCE_OUTPUT_INIT,
    PA_SOURCE_OUTPUT_RUNNING,
    PA_SOURCE_OUTPUT_CORKED,
    PA_SOURCE_OUTPUT_UNLINKED
} pa_source_output_state_t;

static inline pa_bool_t PA_SOURCE_OUTPUT_LINKED(pa_source_output_state_t x) {
    return x == PA_SOURCE_OUTPUT_RUNNING || x == PA_SOURCE_OUTPUT_CORKED;
}

typedef enum pa_source_output_flags {
    PA_SOURCE_OUTPUT_VARIABLE_RATE = 1,
    PA_SOURCE_OUTPUT_DONT_MOVE = 2,
    PA_SOURCE_OUTPUT_START_CORKED = 4,
    PA_SOURCE_OUTPUT_NO_REMAP = 8,
    PA_SOURCE_OUTPUT_NO_REMIX = 16,
    PA_SOURCE_OUTPUT_FIX_FORMAT = 32,
    PA_SOURCE_OUTPUT_FIX_RATE = 64,
    PA_SOURCE_OUTPUT_FIX_CHANNELS = 128
} pa_source_output_flags_t;

struct pa_source_output {
    pa_msgobject parent;

    uint32_t index;
    pa_core *core;
    pa_source_output_state_t state;
    pa_source_output_flags_t flags;

    char *name, *driver;                  /* may be NULL */
    pa_module *module;                    /* may be NULL */
    pa_client *client;                    /* may be NULL */

    pa_source *source;

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    /* Pushes a new memchunk into the output. Called from IO thread
     * context. */
    void (*push)(pa_source_output *o, const pa_memchunk *chunk);

    /* If non-NULL this function is called when the output is first
     * connected to a source. Called from IO thread context */
    void (*attach) (pa_source_output *o);           /* may be NULL */

    /* If non-NULL this function is called when the output is
     * disconnected from its source. Called from IO thread context */
    void (*detach) (pa_source_output *o);           /* may be NULL */

    /* If non-NULL called whenever the the source this output is attached
     * to changes. Called from main context */
    void (*moved) (pa_source_output *o);   /* may be NULL */

    /* If non-NULL called whenever the the source this output is attached
     * to suspends or resumes. Called from main context */
    void (*suspend) (pa_source_output *o, pa_bool_t b);   /* may be NULL */

    /* Supposed to unlink and destroy this stream. Called from main
     * context. */
    void (*kill)(pa_source_output* o);              /* may be NULL */

    /* Return the current latency (i.e. length of bufferd audio) of
    this stream. Called from main context. If NULL a
    PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY message is sent to the IO
    thread instead. */
    pa_usec_t (*get_latency) (pa_source_output *o); /* may be NULL */

    pa_resample_method_t resample_method;

    struct {
        pa_source_output_state_t state;

        pa_bool_t attached; /* True only between ->attach() and ->detach() calls */

        pa_sample_spec sample_spec;

        pa_resampler* resampler;              /* may be NULL */
    } thread_info;

    void *userdata;
};

PA_DECLARE_CLASS(pa_source_output);
#define PA_SOURCE_OUTPUT(o) pa_source_output_cast(o)

enum {
    PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY,
    PA_SOURCE_OUTPUT_MESSAGE_SET_RATE,
    PA_SOURCE_OUTPUT_MESSAGE_SET_STATE,
    PA_SOURCE_OUTPUT_MESSAGE_MAX
};

typedef struct pa_source_output_new_data {
    const char *name, *driver;
    pa_module *module;
    pa_client *client;

    pa_source *source;

    pa_sample_spec sample_spec;
    pa_bool_t sample_spec_is_set;
    pa_channel_map channel_map;
    pa_bool_t channel_map_is_set;

    pa_resample_method_t resample_method;
} pa_source_output_new_data;

typedef struct pa_source_output_move_hook_data {
    pa_source_output *source_output;
    pa_source *destination;
} pa_source_output_move_hook_data;

pa_source_output_new_data* pa_source_output_new_data_init(pa_source_output_new_data *data);
void pa_source_output_new_data_set_sample_spec(pa_source_output_new_data *data, const pa_sample_spec *spec);
void pa_source_output_new_data_set_channel_map(pa_source_output_new_data *data, const pa_channel_map *map);
void pa_source_output_new_data_set_volume(pa_source_output_new_data *data, const pa_cvolume *volume);

/* To be called by the implementing module only */

pa_source_output* pa_source_output_new(
        pa_core *core,
        pa_source_output_new_data *data,
        pa_source_output_flags_t flags);

void pa_source_output_put(pa_source_output *o);
void pa_source_output_unlink(pa_source_output*o);

void pa_source_output_set_name(pa_source_output *i, const char *name);

/* Callable by everyone */

/* External code may request disconnection with this funcion */
void pa_source_output_kill(pa_source_output*o);

pa_usec_t pa_source_output_get_latency(pa_source_output *i);

void pa_source_output_cork(pa_source_output *i, pa_bool_t b);

int pa_source_output_set_rate(pa_source_output *o, uint32_t rate);

pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o);

int pa_source_output_move_to(pa_source_output *o, pa_source *dest);

#define pa_source_output_get_state(o) ((o)->state)

/* To be used exclusively by the source driver thread */

void pa_source_output_push(pa_source_output *o, const pa_memchunk *chunk);
int pa_source_output_process_msg(pa_msgobject *mo, int code, void *userdata, int64_t offset, pa_memchunk *chunk);

#endif
