#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "memchunk.h"

void memchunk_make_writable(struct memchunk *c) {
    struct memblock *n;
    assert(c && c->memblock && c->memblock->ref >= 1);

    if (c->memblock->ref == 1)
        return;
    
    n = memblock_new(c->length);
    assert(n);
    memcpy(n->data, c->memblock->data+c->index, c->length);
    memblock_unref(c->memblock);
    c->memblock = n;
    c->index = 0;
}


struct mcalign {
    size_t base;
    struct memchunk chunk;
    uint8_t *buffer;
    size_t buffer_fill;
};

struct mcalign *mcalign_new(size_t base) {
    struct mcalign *m;
    assert(base);

    m = malloc(sizeof(struct mcalign));
    assert(m);
    m->base = base;
    m->chunk.memblock = NULL;
    m->chunk.length = m->chunk.index = 0;
    m->buffer = NULL;
    m->buffer_fill = 0;
    return m;
}

void mcalign_free(struct mcalign *m) {
    assert(m);

    free(m->buffer);
    
    if (m->chunk.memblock)
        memblock_unref(m->chunk.memblock);
    
    free(m);
}

void mcalign_push(struct mcalign *m, const struct memchunk *c) {
    assert(m && c && !m->chunk.memblock && c->memblock && c->length);

    m->chunk = *c;
    memblock_ref(m->chunk.memblock);
}

int mcalign_pop(struct mcalign *m, struct memchunk *c) {
    assert(m && c && m->base > m->buffer_fill);
    int ret;

    if (!m->chunk.memblock)
        return -1;

    if (m->buffer_fill) {
        size_t l = m->base - m->buffer_fill;
        if (l > m->chunk.length)
            l = m->chunk.length;
        assert(m->buffer && l);

        memcpy(m->buffer + m->buffer_fill, m->chunk.memblock->data + m->chunk.index, l);
        m->buffer_fill += l;
        m->chunk.index += l;
        m->chunk.length -= l;

        if (m->chunk.length == 0) {
            m->chunk.length = m->chunk.index = 0;
            memblock_unref(m->chunk.memblock);
            m->chunk.memblock = NULL;
        }

        assert(m->buffer_fill <= m->base);
        if (m->buffer_fill == m->base) {
            c->memblock = memblock_new_dynamic(m->buffer, m->base);
            assert(c->memblock);
            c->index = 0;
            c->length = m->base;
            m->buffer = NULL;
            m->buffer_fill = 0;

            return 0;
        }

        return -1;
    }

    m->buffer_fill = m->chunk.length % m->base;

    if (m->buffer_fill) {
        assert(!m->buffer);
        m->buffer = malloc(m->base);
        assert(m->buffer);
        m->chunk.length -= m->buffer_fill;
        memcpy(m->buffer, m->chunk.memblock->data + m->chunk.index + m->chunk.length, m->buffer_fill);
    }

    if (m->chunk.length) {
        *c = m->chunk;
        memblock_ref(c->memblock);
        ret = 0;
    } else
        ret = -1;
    
    m->chunk.length = m->chunk.index = 0;
    memblock_unref(m->chunk.memblock);
    m->chunk.memblock = NULL;

    return ret;
}
