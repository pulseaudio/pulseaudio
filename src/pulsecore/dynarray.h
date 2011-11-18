#ifndef foopulsecoredynarrayhfoo
#define foopulsecoredynarrayhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/def.h>

typedef struct pa_dynarray pa_dynarray;

/* Implementation of a simple dynamically sized array. The array
 * expands if required, but doesn't shrink if possible. Memory
 * management of the array's entries is the user's job. */

pa_dynarray* pa_dynarray_new(void);

/* Free the array calling the specified function for every entry in
 * the array. The function may be NULL. */
void pa_dynarray_free(pa_dynarray *a, pa_free_cb_t free_func);

/* Store p at position i in the array */
void pa_dynarray_put(pa_dynarray*a, unsigned i, void *p);

/* Store p a the first free position in the array. Returns the index
 * of that entry. If entries are removed from the array their position
 * are not filled any more by this function. */
unsigned pa_dynarray_append(pa_dynarray*a, void *p);

void *pa_dynarray_get(pa_dynarray*a, unsigned i);

unsigned pa_dynarray_size(pa_dynarray*a);

#endif
