#ifndef foosinkinputhfoo
#define foosinkinputhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
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

struct pa_sink_input {
    uint32_t index;

    int corked;
    
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

    void *userdata;

    struct pa_memchunk resampled_chunk;
    struct pa_resampler *resampler;
};

struct pa_sink_input* pa_sink_input_new(struct pa_sink *s, const char *name, const struct pa_sample_spec *spec);
void pa_sink_input_free(struct pa_sink_input* i);

/* Code that didn't create the input stream should call this function to
 * request destruction of it */
void pa_sink_input_kill(struct pa_sink_input *i);

pa_usec_t pa_sink_input_get_latency(struct pa_sink_input *i);

int pa_sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk);
void pa_sink_input_drop(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length);

void pa_sink_input_set_volume(struct pa_sink_input *i, pa_volume_t volume);

void pa_sink_input_cork(struct pa_sink_input *i, int b);

#endif
