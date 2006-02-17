#ifndef foomemblockqhfoo
#define foomemblockqhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>

#include <polypcore/memblock.h>
#include <polypcore/memchunk.h>

/* A memblockq is a queue of pa_memchunks (yepp, the name is not
 * perfect). It is similar to the ring buffers used by most other
 * audio software. In contrast to a ring buffer this memblockq data
 * type doesn't need to copy any data around, it just maintains
 * references to reference counted memory blocks. */

typedef struct pa_memblockq pa_memblockq;

/* Parameters:
   - maxlength: maximum length of queue. If more data is pushed into the queue, data from the front is dropped
   - length:    the target length of the queue.
   - base:      a base value for all metrics. Only multiples of this value are popped from the queue
   - prebuf:    before passing the first byte out, make sure that enough bytes are in the queue
   - minreq:    pa_memblockq_missing() will only return values greater than this value
*/
pa_memblockq* pa_memblockq_new(size_t maxlength,
                                      size_t tlength,
                                      size_t base,
                                      size_t prebuf,
                                      size_t minreq,
                                      pa_memblock_stat *s);
void pa_memblockq_free(pa_memblockq*bq);

/* Push a new memory chunk into the queue. Optionally specify a value for future cancellation. */
void pa_memblockq_push(pa_memblockq* bq, const pa_memchunk *chunk, size_t delta);

/* Same as pa_memblockq_push(), however chunks are filtered through a mcalign object, and thus aligned to multiples of base */
void pa_memblockq_push_align(pa_memblockq* bq, const pa_memchunk *chunk, size_t delta);

/* Return a copy of the next memory chunk in the queue. It is not removed from the queue */
int pa_memblockq_peek(pa_memblockq* bq, pa_memchunk *chunk);

/* Drop the specified bytes from the queue, only valid aufter pa_memblockq_peek() */
void pa_memblockq_drop(pa_memblockq *bq, const pa_memchunk *chunk, size_t length);

/* Drop the specified bytes from the queue */
void pa_memblockq_skip(pa_memblockq *bq, size_t length);

/* Shorten the pa_memblockq to the specified length by dropping data at the end of the queue */
void pa_memblockq_shorten(pa_memblockq *bq, size_t length);

/* Empty the pa_memblockq */
void pa_memblockq_empty(pa_memblockq *bq);

/* Test if the pa_memblockq is currently readable, that is, more data than base */
int pa_memblockq_is_readable(pa_memblockq *bq);

/* Test if the pa_memblockq is currently writable for the specified amount of bytes */
int pa_memblockq_is_writable(pa_memblockq *bq, size_t length);

/* Return the length of the queue in bytes */
uint32_t pa_memblockq_get_length(pa_memblockq *bq);

/* Return how many bytes are missing in queue to the specified fill amount */
uint32_t pa_memblockq_missing(pa_memblockq *bq);

/* Returns the minimal request value */
uint32_t pa_memblockq_get_minreq(pa_memblockq *bq);

/* Force disabling of pre-buf even when the pre-buffer is not yet filled */
void pa_memblockq_prebuf_disable(pa_memblockq *bq);

/* Reenable pre-buf to the initial level */
void pa_memblockq_prebuf_reenable(pa_memblockq *bq);

/* Manipulate the write pointer */
void pa_memblockq_seek(pa_memblockq *bq, size_t delta);

/* Flush the queue */
void pa_memblockq_flush(pa_memblockq *bq);

/* Get Target length */
uint32_t pa_memblockq_get_tlength(pa_memblockq *bq);

#endif
