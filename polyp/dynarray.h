#ifndef foodynarrayhfoo
#define foodynarrayhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

struct pa_dynarray;

/* Implementation of a simple dynamically sized array. The array
 * expands if required, but doesn't shrink if possible. Memory
 * management of the array's entries is the user's job. */

struct pa_dynarray* pa_dynarray_new(void);

/* Free the array calling the specified function for every entry in
 * the array. The function may be NULL. */
void pa_dynarray_free(struct pa_dynarray* a, void (*func)(void *p, void *userdata), void *userdata);

/* Store p at position i in the array */
void pa_dynarray_put(struct pa_dynarray*a, unsigned i, void *p);

/* Store p a the first free position in the array. Returns the index
 * of that entry. If entries are removed from the array their position
 * are not filled any more by this function. */
unsigned pa_dynarray_append(struct pa_dynarray*a, void *p);

void *pa_dynarray_get(struct pa_dynarray*a, unsigned i);

unsigned pa_dynarray_ncontents(struct pa_dynarray*a);

#endif
