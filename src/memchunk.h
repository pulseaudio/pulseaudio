#ifndef foomemchunkhfoo
#define foomemchunkhfoo

#include "memblock.h"

struct pa_memchunk {
    struct pa_memblock *memblock;
    size_t index, length;
};

void pa_memchunk_make_writable(struct pa_memchunk *c);

struct pa_mcalign;

struct pa_mcalign *pa_mcalign_new(size_t base);
void pa_mcalign_free(struct pa_mcalign *m);
void pa_mcalign_push(struct pa_mcalign *m, const struct pa_memchunk *c);
int pa_mcalign_pop(struct pa_mcalign *m, struct pa_memchunk *c);

#endif
