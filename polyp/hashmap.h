#ifndef foohashmaphfoo
#define foohashmaphfoo

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

struct pa_hashmap;

struct pa_hashmap *pa_hashmap_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b));
void pa_hashmap_free(struct pa_hashmap*, void (*free_func)(void *p, void *userdata), void *userdata);

int pa_hashmap_put(struct pa_hashmap *h, const void *key, void *value);
void* pa_hashmap_get(struct pa_hashmap *h, const void *key);

void* pa_hashmap_remove(struct pa_hashmap *h, const void *key);

unsigned pa_hashmap_ncontents(struct pa_hashmap *h);

/* May be used to iterate through the hashmap. Initially the opaque
   pointer *state has to be set to NULL. The hashmap may not be
   modified during iteration. The key of the entry is returned in
   *key, if key is non-NULL. After the last entry in the hashmap NULL
   is returned. */
void *pa_hashmap_iterate(struct pa_hashmap *h, void **state, const void**key);

#endif
