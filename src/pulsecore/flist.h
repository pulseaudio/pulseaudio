#ifndef foopulseflisthfoo
#define foopulseflisthfoo

/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/def.h>

/* A multiple-reader multipler-write lock-free free list implementation */

typedef struct pa_flist pa_flist;

/* Size is required to be a power of two, or 0 for the default size */
pa_flist * pa_flist_new(unsigned size);
void pa_flist_free(pa_flist *l, pa_free_cb_t free_cb);

/* Please note that this routine might fail! */
int pa_flist_push(pa_flist*l, void *p);
void* pa_flist_pop(pa_flist*l);

#endif
