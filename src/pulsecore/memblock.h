#ifndef foomemblockhfoo
#define foomemblockhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <sys/types.h>
#include <inttypes.h>

/* A pa_memblock is a reference counted memory block. PulseAudio
 * passed references to pa_memblocks around instead of copying
 * data. See pa_memchunk for a structure that describes parts of
 * memory blocks. */

/* The type of memory this block points to */
typedef enum pa_memblock_type {
    PA_MEMBLOCK_FIXED,     /* data is a pointer to fixed memory that needs not to be freed */
    PA_MEMBLOCK_APPENDED,  /* The most common kind: the data is appended to the memory block */ 
    PA_MEMBLOCK_DYNAMIC,   /* data is a pointer to some memory allocated with pa_xmalloc() */
    PA_MEMBLOCK_USER       /* User supplied memory, to be freed with free_cb */
} pa_memblock_type_t;

/* A structure of keeping memory block statistics */
/* Maintains statistics about memory blocks */
typedef struct pa_memblock_stat {
    int ref;
    unsigned total;
    unsigned total_size;
    unsigned allocated;
    unsigned allocated_size;
} pa_memblock_stat;

typedef struct pa_memblock {
    pa_memblock_type_t type;
    unsigned ref;  /* the reference counter */
    int read_only; /* boolean */
    size_t length;
    void *data;
    void (*free_cb)(void *p);  /* If type == PA_MEMBLOCK_USER this points to a function for freeing this memory block */
    pa_memblock_stat *stat;
} pa_memblock;

/* Allocate a new memory block of type PA_MEMBLOCK_APPENDED */
pa_memblock *pa_memblock_new(size_t length, pa_memblock_stat*s);

/* Allocate a new memory block of type PA_MEMBLOCK_DYNAMIC. The pointer data is to be maintained be the memory block */
pa_memblock *pa_memblock_new_dynamic(void *data, size_t length, pa_memblock_stat*s);

/* Allocate a new memory block of type PA_MEMBLOCK_FIXED */
pa_memblock *pa_memblock_new_fixed(void *data, size_t length, int read_only, pa_memblock_stat*s);

/* Allocate a new memory block of type PA_MEMBLOCK_USER */
pa_memblock *pa_memblock_new_user(void *data, size_t length, void (*free_cb)(void *p), int read_only, pa_memblock_stat*s);

void pa_memblock_unref(pa_memblock*b);
pa_memblock* pa_memblock_ref(pa_memblock*b);

/* This special unref function has to be called by the owner of the
memory of a static memory block when he wants to release all
references to the memory. This causes the memory to be copied and
converted into a PA_MEMBLOCK_DYNAMIC type memory block */
void pa_memblock_unref_fixed(pa_memblock*b);

pa_memblock_stat* pa_memblock_stat_new(void);
void pa_memblock_stat_unref(pa_memblock_stat *s);
pa_memblock_stat * pa_memblock_stat_ref(pa_memblock_stat *s);

#endif
