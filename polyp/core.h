#ifndef foocorehfoo
#define foocorehfoo

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

#include "idxset.h"
#include "hashmap.h"
#include "mainloop-api.h"
#include "sample.h"

struct pa_core {
    struct pa_mainloop_api *mainloop;

    struct pa_idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules, *scache_idxset;

    struct pa_hashmap *namereg, *scache_hashmap;
    
    uint32_t default_source_index, default_sink_index;

    struct pa_sample_spec default_sample_spec;
};

struct pa_core* pa_core_new(struct pa_mainloop_api *m);
void pa_core_free(struct pa_core*c);

#endif
