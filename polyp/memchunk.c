/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "memchunk.h"
#include "xmalloc.h"

void pa_memchunk_make_writable(struct pa_memchunk *c, struct pa_memblock_stat *s) {
    struct pa_memblock *n;
    assert(c && c->memblock && c->memblock->ref >= 1);

    if (c->memblock->ref == 1)
        return;
    
    n = pa_memblock_new(c->length, s);
    assert(n);
    memcpy(n->data, (uint8_t*) c->memblock->data+c->index, c->length);
    pa_memblock_unref(c->memblock);
    c->memblock = n;
    c->index = 0;
}


struct pa_mcalign {
    size_t base;
    struct pa_memchunk chunk;
    uint8_t *buffer;
    size_t buffer_fill;
    struct pa_memblock_stat *memblock_stat;
};

struct pa_mcalign *pa_mcalign_new(size_t base, struct pa_memblock_stat *s) {
    struct pa_mcalign *m;
    assert(base);

    m = pa_xmalloc(sizeof(struct pa_mcalign));
    m->base = base;
    m->chunk.memblock = NULL;
    m->chunk.length = m->chunk.index = 0;
    m->buffer = NULL;
    m->buffer_fill = 0;
    m->memblock_stat = s;
    return m;
}

void pa_mcalign_free(struct pa_mcalign *m) {
    assert(m);

    pa_xfree(m->buffer);
    
    if (m->chunk.memblock)
        pa_memblock_unref(m->chunk.memblock);
    
    pa_xfree(m);
}

void pa_mcalign_push(struct pa_mcalign *m, const struct pa_memchunk *c) {
    assert(m && c && !m->chunk.memblock && c->memblock && c->length);

    m->chunk = *c;
    pa_memblock_ref(m->chunk.memblock);
}

int pa_mcalign_pop(struct pa_mcalign *m, struct pa_memchunk *c) {
    int ret;
    assert(m && c && m->base > m->buffer_fill);

    if (!m->chunk.memblock)
        return -1;

    if (m->buffer_fill) {
        size_t l = m->base - m->buffer_fill;
        if (l > m->chunk.length)
            l = m->chunk.length;
        assert(m->buffer && l);

        memcpy((uint8_t*) m->buffer + m->buffer_fill, (uint8_t*) m->chunk.memblock->data + m->chunk.index, l);
        m->buffer_fill += l;
        m->chunk.index += l;
        m->chunk.length -= l;

        if (m->chunk.length == 0) {
            m->chunk.length = m->chunk.index = 0;
            pa_memblock_unref(m->chunk.memblock);
            m->chunk.memblock = NULL;
        }

        assert(m->buffer_fill <= m->base);
        if (m->buffer_fill == m->base) {
            c->memblock = pa_memblock_new_dynamic(m->buffer, m->base, m->memblock_stat);
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
        m->buffer = pa_xmalloc(m->base);
        m->chunk.length -= m->buffer_fill;
        memcpy(m->buffer, (uint8_t*) m->chunk.memblock->data + m->chunk.index + m->chunk.length, m->buffer_fill);
    }

    if (m->chunk.length) {
        *c = m->chunk;
        pa_memblock_ref(c->memblock);
        ret = 0;
    } else
        ret = -1;
    
    m->chunk.length = m->chunk.index = 0;
    pa_memblock_unref(m->chunk.memblock);
    m->chunk.memblock = NULL;

    return ret;
}
