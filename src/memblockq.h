#ifndef foomemblockqhfoo
#define foomemblockqhfoo

#include <sys/types.h>

#include "memblock.h"
#include "memchunk.h"

struct memblockq;

/* Parameters: the maximum length of the memblock queue, a base value
for all operations (that is, all byte operations shall work on
multiples of this base value) and an amount of bytes to prebuffer
before having memblockq_peek() succeed. */
struct memblockq* memblockq_new(size_t maxlength, size_t base, size_t prebuf);
void memblockq_free(struct memblockq*bq);

/* Push a new memory chunk into the queue. Optionally specify a value for future cancellation. This is currently not implemented, however! */
void memblockq_push(struct memblockq* bq, const struct memchunk *chunk, size_t delta);

/* Same as memblockq_push(), however chunks are filtered through a mcalign object, and thus aligned to multiples of base */
void memblockq_push_align(struct memblockq* bq, const struct memchunk *chunk, size_t delta);

/* Return a copy of the next memory chunk in the queue. It is not removed from the queue */
int memblockq_peek(struct memblockq* bq, struct memchunk *chunk);

/* Drop the specified bytes from the queue */
void memblockq_drop(struct memblockq *bq, size_t length);

/* Shorten the memblockq to the specified length by dropping data at the end of the queue */
void memblockq_shorten(struct memblockq *bq, size_t length);

/* Empty the memblockq */
void memblockq_empty(struct memblockq *bq);

/* Test if the memblockq is currently readable, that is, more data than base */
int memblockq_is_readable(struct memblockq *bq);

/* Test if the memblockq is currently writable for the specified amount of bytes */
int memblockq_is_writable(struct memblockq *bq, size_t length);

/* The time memory chunks stay in the queue until they are removed completely in usecs */
uint32_t memblockq_get_delay(struct memblockq *bq);

/* Return the length of the queue in bytes */
uint32_t memblockq_get_length(struct memblockq *bq);

/* Return how many bytes are missing in queue to the specified fill amount */
uint32_t memblockq_missing_to(struct memblockq *bq, size_t qlen);

#endif
