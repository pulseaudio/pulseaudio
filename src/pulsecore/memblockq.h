#ifndef foomemblockqhfoo
#define foomemblockqhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <sys/types.h>
#include <inttypes.h>

#include <pulsecore/memblock.h>
#include <pulsecore/memchunk.h>
#include <pulse/def.h>

/* A memblockq is a queue of pa_memchunks (yepp, the name is not
 * perfect). It is similar to the ring buffers used by most other
 * audio software. In contrast to a ring buffer this memblockq data
 * type doesn't need to copy any data around, it just maintains
 * references to reference counted memory blocks. */

typedef struct pa_memblockq pa_memblockq;


/* Parameters:

   - idx:       start value for both read and write index

   - maxlength: maximum length of queue. If more data is pushed into
                the queue, the operation will fail. Must not be 0.

   - tlength:   the target length of the queue. Pass 0 for the default.

   - base:      a base value for all metrics. Only multiples of this value
                are popped from the queue or should be pushed into
                it. Must not be 0.

   - prebuf:    If the queue runs empty wait until this many bytes are in
                queue again before passing the first byte out. If set
                to 0 pa_memblockq_pop() will return a silence memblock
                if no data is in the queue and will never fail. Pass
                (size_t) -1 for the default.

   - minreq:    pa_memblockq_missing() will only return values greater
                than this value. Pass 0 for the default.

   - silence:   return this memblock when reading unitialized data
*/
pa_memblockq* pa_memblockq_new(
        int64_t idx,
        size_t maxlength,
        size_t tlength,
        size_t base,
        size_t prebuf,
        size_t minreq,
        pa_memblock *silence);

void pa_memblockq_free(pa_memblockq*bq);

/* Push a new memory chunk into the queue.  */
int pa_memblockq_push(pa_memblockq* bq, const pa_memchunk *chunk);

/* Push a new memory chunk into the queue, but filter it through a
 * pa_mcalign object. Don't mix this with pa_memblockq_seek() unless
 * you know what you do. */
int pa_memblockq_push_align(pa_memblockq* bq, const pa_memchunk *chunk);

/* Return a copy of the next memory chunk in the queue. It is not
 * removed from the queue. There are two reasons this function might
 * fail: 1. prebuffering is active, 2. queue is empty and no silence
 * memblock was passed at initialization. If the queue is not empty,
 * but we're currently at a hole in the queue and no silence memblock
 * was passed we return the length of the hole in chunk->length. */
int pa_memblockq_peek(pa_memblockq* bq, pa_memchunk *chunk);

/* Drop the specified bytes from the queue. */
void pa_memblockq_drop(pa_memblockq *bq, size_t length);

/* Test if the pa_memblockq is currently readable, that is, more data than base */
int pa_memblockq_is_readable(pa_memblockq *bq);

/* Return the length of the queue in bytes */
size_t pa_memblockq_get_length(pa_memblockq *bq);

/* Return how many bytes are missing in queue to the specified fill amount */
size_t pa_memblockq_missing(pa_memblockq *bq);

/* Return the number of bytes that are missing since the last call to
 * this function, reset the internal counter to 0. */
size_t pa_memblockq_pop_missing(pa_memblockq *bq);

/* Returns the minimal request value */
size_t pa_memblockq_get_minreq(pa_memblockq *bq);

/* Manipulate the write pointer */
void pa_memblockq_seek(pa_memblockq *bq, int64_t offset, pa_seek_mode_t seek);

/* Set the queue to silence, set write index to read index */
void pa_memblockq_flush(pa_memblockq *bq);

/* Get Target length */
size_t pa_memblockq_get_tlength(pa_memblockq *bq);

/* Return the current read index */
int64_t pa_memblockq_get_read_index(pa_memblockq *bq);

/* Return the current write index */
int64_t pa_memblockq_get_write_index(pa_memblockq *bq);

/* Shorten the pa_memblockq to the specified length by dropping data
 * at the read end of the queue. The read index is increased until the
 * queue has the specified length */
void pa_memblockq_shorten(pa_memblockq *bq, size_t length);

/* Ignore prebuf for now */
void pa_memblockq_prebuf_disable(pa_memblockq *bq);

/* Force prebuf */
void pa_memblockq_prebuf_force(pa_memblockq *bq);

/* Return the maximum length of the queue in bytes */
size_t pa_memblockq_get_maxlength(pa_memblockq *bq);

/* Return the prebuffer length in bytes */
size_t pa_memblockq_get_prebuf(pa_memblockq *bq);

/* Change metrics. */
void pa_memblockq_set_maxlength(pa_memblockq *memblockq, size_t maxlength);
void pa_memblockq_set_tlength(pa_memblockq *memblockq, size_t tlength);
void pa_memblockq_set_prebuf(pa_memblockq *memblockq, size_t prebuf);
void pa_memblockq_set_minreq(pa_memblockq *memblockq, size_t minreq);

#endif
