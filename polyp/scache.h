#ifndef fooscachehfoo
#define fooscachehfoo

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

#include "core.h"
#include "memchunk.h"
#include "sink.h"

struct pa_scache_entry {
    struct pa_core *core;
    uint32_t index;
    char *name;
    uint32_t volume;
    struct pa_sample_spec sample_spec;
    struct pa_memchunk memchunk;
};

int pa_scache_add_item(struct pa_core *c, const char *name, struct pa_sample_spec *ss, struct pa_memchunk *chunk, uint32_t *index);

int pa_scache_remove_item(struct pa_core *c, const char *name);
int pa_scache_play_item(struct pa_core *c, const char *name, struct pa_sink *sink, uint32_t volume);
void pa_scache_free(struct pa_core *c);

const char *pa_scache_get_name_by_id(struct pa_core *c, uint32_t id);
uint32_t pa_scache_get_id_by_name(struct pa_core *c, const char *name);

#endif
