#ifndef foosourceoutputhfoo
#define foosourceoutputhfoo

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

typedef struct pa_source_output pa_source_output;

#include <polyp/sample.h>
#include <polypcore/source.h>
#include <polypcore/memblockq.h>
#include <polypcore/resampler.h>
#include <polypcore/module.h>
#include <polypcore/client.h>

typedef enum {
    PA_SOURCE_OUTPUT_RUNNING,
    PA_SOURCE_OUTPUT_CORKED,
    PA_SOURCE_OUTPUT_DISCONNECTED
} pa_source_output_state_t;

struct pa_source_output {
    int ref;
    uint32_t index;
    pa_source_output_state_t state;
    
    char *name, *driver;
    pa_module *owner;

    pa_source *source;
    pa_client *client;

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    
    void (*push)(pa_source_output *o, const pa_memchunk *chunk);
    void (*kill)(pa_source_output* o);
    pa_usec_t (*get_latency) (pa_source_output *o);

    pa_resampler* resampler;
    
    void *userdata;
};

pa_source_output* pa_source_output_new(
    pa_source *s,
    const char *driver,
    const char *name,
    const pa_sample_spec *spec,
    const pa_channel_map *map,
    int resample_method);

void pa_source_output_unref(pa_source_output* o);
pa_source_output* pa_source_output_ref(pa_source_output *o);

/* To be called by the implementing module only */
void pa_source_output_disconnect(pa_source_output*o);

/* External code may request disconnection with this funcion */
void pa_source_output_kill(pa_source_output*o);

void pa_source_output_push(pa_source_output *o, const pa_memchunk *chunk);

void pa_source_output_set_name(pa_source_output *i, const char *name);

pa_usec_t pa_source_output_get_latency(pa_source_output *i);

void pa_source_output_cork(pa_source_output *i, int b);

pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o);

#endif
