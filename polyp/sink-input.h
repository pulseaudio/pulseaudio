#ifndef foosinkinputhfoo
#define foosinkinputhfoo

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

#include "sink.h"
#include "sample.h"
#include "memblockq.h"
#include "resampler.h"
#include "module.h"
#include "client.h"

enum pa_sink_input_state {
    PA_SINK_INPUT_RUNNING,
    PA_SINK_INPUT_CORKED,
    PA_SINK_INPUT_DISCONNECTED
};

struct pa_sink_input {
    int ref;
    enum pa_sink_input_state state;
    
    uint32_t index;

    char *name;
    struct pa_module *owner;
    struct pa_client *client;
    struct pa_sink *sink;
    struct pa_sample_spec sample_spec;
    uint32_t volume;
    
    int (*peek) (struct pa_sink_input *i, struct pa_memchunk *chunk);
    void (*drop) (struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length);
    void (*kill) (struct pa_sink_input *i);
    pa_usec_t (*get_latency) (struct pa_sink_input *i);
    void (*underrun) (struct pa_sink_input *i);

    void *userdata;

    int playing;

    struct pa_memchunk resampled_chunk;
    struct pa_resampler *resampler;
};

struct pa_sink_input* pa_sink_input_new(struct pa_sink *s, const char *name, const struct pa_sample_spec *spec, int variable_rate, int resample_method);
void pa_sink_input_unref(struct pa_sink_input* i);
struct pa_sink_input* pa_sink_input_ref(struct pa_sink_input* i);

/* To be called by the implementing module only */
void pa_sink_input_disconnect(struct pa_sink_input* i);

/* External code may request disconnection with this funcion */
void pa_sink_input_kill(struct pa_sink_input*i);

pa_usec_t pa_sink_input_get_latency(struct pa_sink_input *i);

int pa_sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk);
void pa_sink_input_drop(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length);

void pa_sink_input_set_volume(struct pa_sink_input *i, pa_volume_t volume);

void pa_sink_input_cork(struct pa_sink_input *i, int b);

void pa_sink_input_set_rate(struct pa_sink_input *i, uint32_t rate);

void pa_sink_input_set_name(struct pa_sink_input *i, const char *name);

#endif
