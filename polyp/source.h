#ifndef foosourcehfoo
#define foosourcehfoo

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

struct pa_source;

#include <inttypes.h>
#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "memblock.h"
#include "memchunk.h"
#include "sink.h"

#define PA_MAX_OUTPUTS_PER_SOURCE 16

enum pa_source_state {
    PA_SOURCE_RUNNING,
    PA_SOURCE_DISCONNECTED
};

struct pa_source {
    int ref;
    enum pa_source_state state;
    
    uint32_t index;
    
    char *name, *description;
    struct pa_module *owner;
    struct pa_core *core;
    struct pa_sample_spec sample_spec;
    struct pa_idxset *outputs;
    struct pa_sink *monitor_of;

    void (*notify)(struct pa_source*source);
    pa_usec_t (*get_latency)(struct pa_source *s);
    void *userdata;
};

struct pa_source* pa_source_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec);
void pa_source_disconnect(struct pa_source *s);
void pa_source_unref(struct pa_source *s);
struct pa_source* pa_source_ref(struct pa_source *c);

/* Pass a new memory block to all output streams */
void pa_source_post(struct pa_source*s, struct pa_memchunk *b);

void pa_source_notify(struct pa_source *s);

void pa_source_set_owner(struct pa_source *s, struct pa_module *m);

pa_usec_t pa_source_get_latency(struct pa_source *s);

#endif
