#ifndef foomemblockqhfoo
#define foomemblockqhfoo

#include <sys/types.h>

#include "memblock.h"

struct memblockq;

struct memblockq* memblockq_new(size_t maxlength, size_t base);
void memblockq_free(struct memblockq* bq);

void memblockq_push(struct memblockq* bq, struct memchunk *chunk, size_t delta);

int memblockq_pop(struct memblockq* bq, struct memchunk *chunk);
int memblockq_peek(struct memblockq* bq, struct memchunk *chunk);
void memblockq_drop(struct memblockq *bq, size_t length);

void memblockq_shorten(struct memblockq *bq, size_t length);
void memblockq_empty(struct memblockq *bq);

int memblockq_is_empty(struct memblockq *bq);

#endif
