#ifndef fooidxsethfoo
#define fooidxsethfoo

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

#define PA_IDXSET_INVALID ((uint32_t) -1)

unsigned pa_idxset_trivial_hash_func(const void *p);
int pa_idxset_trivial_compare_func(const void *a, const void *b);

unsigned pa_idxset_string_hash_func(const void *p);
int pa_idxset_string_compare_func(const void *a, const void *b);

struct pa_idxset;

struct pa_idxset* pa_idxset_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b));
void pa_idxset_free(struct pa_idxset *s, void (*free_func) (void *p, void *userdata), void *userdata);

int pa_idxset_put(struct pa_idxset*s, void *p, uint32_t *index);

void* pa_idxset_get_by_index(struct pa_idxset*s, uint32_t index);
void* pa_idxset_get_by_data(struct pa_idxset*s, void *p, uint32_t *index);

void* pa_idxset_remove_by_index(struct pa_idxset*s, uint32_t index);
void* pa_idxset_remove_by_data(struct pa_idxset*s, void *p, uint32_t *index);

/* This may be used to iterate through all entries. When called with
   an invalid index value it returns the first entry, otherwise the
   next following. The function is best called with *index =
   PA_IDXSET_VALID first. */
void* pa_idxset_rrobin(struct pa_idxset *s, uint32_t *index);

/* Return the oldest entry in the idxset */
void* pa_idxset_first(struct pa_idxset *s, uint32_t *index);
void *pa_idxset_next(struct pa_idxset *s, uint32_t *index);

int pa_idxset_foreach(struct pa_idxset*s, int (*func)(void *p, uint32_t index, int *del, void*userdata), void *userdata);

unsigned pa_idxset_ncontents(struct pa_idxset*s);
int pa_idxset_isempty(struct pa_idxset *s);

#endif
