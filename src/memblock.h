#ifndef foomemblockhfoo
#define foomemblockhfoo

#include <sys/types.h>

enum memblock_type { MEMBLOCK_FIXED, MEMBLOCK_APPENDED, MEMBLOCK_DYNAMIC };

struct memblock {
    enum memblock_type type;
    unsigned ref;
    size_t length;
    void *data;
};

struct memchunk {
    struct memblock *memblock;
    size_t index, length;
};

struct memblock *memblock_new(size_t length);
struct memblock *memblock_new_fixed(void *data, size_t length);
struct memblock *memblock_new_dynamic(void *data, size_t length);

void memblock_unref(struct memblock*b);
struct memblock* memblock_ref(struct memblock*b);

void memblock_unref_fixed(struct memblock*b);

#define memblock_assert_exclusive(b) assert((b)->ref == 1)

#endif
