#ifndef fooqueuehfoo
#define fooqueuehfoo

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

struct pa_queue;

/* A simple implementation of the abstract data type queue. Stores
 * pointers as members. The memory has to be managed by the caller. */

struct pa_queue* pa_queue_new(void);

/* Free the queue and run the specified callback function for every remaining entry. The callback function may be NULL. */
void pa_queue_free(struct pa_queue* q, void (*destroy)(void *p, void *userdata), void *userdata);

void pa_queue_push(struct pa_queue *q, void *p);
void* pa_queue_pop(struct pa_queue *q);

int pa_queue_is_empty(struct pa_queue *q);

#endif
