#ifndef fooscachehfoo
#define fooscachehfoo

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

    char *filename;
    
    int lazy;
    time_t last_used_time;
};

int pa_scache_add_item(struct pa_core *c, const char *name, struct pa_sample_spec *ss, struct pa_memchunk *chunk, uint32_t *index);
int pa_scache_add_file(struct pa_core *c, const char *name, const char *filename, uint32_t *index);
int pa_scache_add_file_lazy(struct pa_core *c, const char *name, const char *filename, uint32_t *index);

int pa_scache_add_directory_lazy(struct pa_core *c, const char *pathname);

int pa_scache_remove_item(struct pa_core *c, const char *name);
int pa_scache_play_item(struct pa_core *c, const char *name, struct pa_sink *sink, uint32_t volume);
void pa_scache_free(struct pa_core *c);

const char *pa_scache_get_name_by_id(struct pa_core *c, uint32_t id);
uint32_t pa_scache_get_id_by_name(struct pa_core *c, const char *name);

uint32_t pa_scache_total_size(struct pa_core *c);

void pa_scache_unload_unused(struct pa_core *c);

#endif
