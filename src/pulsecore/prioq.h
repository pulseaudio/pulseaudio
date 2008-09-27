#ifndef foopulsecoreprioqhfoo
#define foopulsecoreprioqhfoo

/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering

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

#include <inttypes.h>

#include <pulsecore/macro.h>
#include <pulsecore/idxset.h>

/* A heap-based priority queue. Removal and insertion is O(log
 * n). Removal can happen a the top or at any position referenced by a
 * pa_prioq_item.  */

typedef struct pa_prioq pa_prioq;
typedef struct pa_prioq_item pa_prioq_item;

/* Instantiate a new prioq with the specified comparison functions */
pa_prioq* pa_prioq_new(pa_compare_func_t compare_func);

/* Free the prioq. When the prioq is not empty the specified function is called for every entry contained */
void pa_prioq_free(pa_prioq *q, pa_free2_cb_t free_cb, void *userdata);

/* Store a new item in the prioq. */
pa_prioq_item* pa_prioq_put(pa_prioq *q, void* data);

/* Get the item on the top of the queue, but don't remove it from the queue*/
void* pa_prioq_peek(pa_prioq*q);

/* Get the item on the top of the queue, and remove it from thq queue */
void* pa_prioq_pop(pa_prioq*q);

/* Remove an arbitrary from theq prioq, returning it's data */
void* pa_prioq_remove(pa_prioq*q, pa_prioq_item *i);

/* The priority of an item was modified. Adjustthe queue to that */
void pa_prioq_reshuffle(pa_prioq *q, pa_prioq_item *i);

/* Return the current number of items in the prioq */
unsigned pa_prioq_size(pa_prioq*s);

/* Return TRUE of the prioq is empty */
pa_bool_t pa_prioq_isempty(pa_prioq *s);

#endif
