#ifndef foomemchunkhfoo
#define foomemchunkhfoo

#include "memblock.h"

struct memchunk {
    struct memblock *memblock;
    size_t index, length;
};

void memchunk_make_writable(struct memchunk *c);

struct mcalign;

struct mcalign *mcalign_new(size_t base);
void mcalign_free(struct mcalign *m);
void mcalign_push(struct mcalign *m, const struct memchunk *c);
int mcalign_pop(struct mcalign *m, struct memchunk *c);

#endif
