#ifndef foomemblockhfoo
#define foomemblockhfoo

#include <sys/types.h>
#include <inttypes.h>

enum pa_memblock_type { PA_MEMBLOCK_FIXED, PA_MEMBLOCK_APPENDED, PA_MEMBLOCK_DYNAMIC };

struct pa_memblock {
    enum pa_memblock_type type;
    unsigned ref;
    size_t length;
    void *data;
};

struct pa_memblock *pa_memblock_new(size_t length);
struct pa_memblock *pa_memblock_new_fixed(void *data, size_t length);
struct pa_memblock *pa_memblock_new_dynamic(void *data, size_t length);

void pa_memblock_unref(struct pa_memblock*b);
struct pa_memblock* pa_memblock_ref(struct pa_memblock*b);

void pa_memblock_unref_fixed(struct pa_memblock*b);

extern unsigned pa_memblock_count, pa_memblock_total;

#endif
