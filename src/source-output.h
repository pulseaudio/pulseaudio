#ifndef foosourceoutputhfoo
#define foosourceoutputhfoo

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

#include "source.h"
#include "sample.h"
#include "memblockq.h"
#include "resampler.h"
#include "module.h"
#include "client.h"

struct pa_source_output {
    uint32_t index;

    char *name;
    struct pa_module *owner;
    struct pa_client *client;
    struct pa_source *source;
    struct pa_sample_spec sample_spec;
    
    void (*push)(struct pa_source_output *o, const struct pa_memchunk *chunk);
    void (*kill)(struct pa_source_output* o);

    struct pa_resampler* resampler;
    
    void *userdata;
};

struct pa_source_output* pa_source_output_new(struct pa_source *s, const char *name, const struct pa_sample_spec *spec);
void pa_source_output_free(struct pa_source_output* o);

void pa_source_output_kill(struct pa_source_output*o);

void pa_source_output_push(struct pa_source_output *o, const struct pa_memchunk *chunk);

#endif
