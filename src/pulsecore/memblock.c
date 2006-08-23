/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
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
#include <unistd.h>

#include <pulse/xmalloc.h>

#include <pulsecore/shm.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>

#include "memblock.h"

#define PA_MEMPOOL_SLOTS_MAX 128
#define PA_MEMPOOL_SLOT_SIZE (16*1024)

#define PA_MEMEXPORT_SLOTS_MAX 128

#define PA_MEMIMPORT_SLOTS_MAX 128
#define PA_MEMIMPORT_SEGMENTS_MAX 16

struct pa_memimport_segment {
    pa_memimport *import;
    pa_shm memory;
    unsigned n_blocks;
};

struct pa_memimport {
    pa_mempool *pool;
    pa_hashmap *segments;
    pa_hashmap *blocks;

    /* Called whenever an imported memory block is no longer
     * needed. */
    pa_memimport_release_cb_t release_cb;
    void *userdata;

    PA_LLIST_FIELDS(pa_memimport);
};

struct memexport_slot {
    PA_LLIST_FIELDS(struct memexport_slot);
    pa_memblock *block;
};

struct pa_memexport {
    pa_mempool *pool;
    
    struct memexport_slot slots[PA_MEMEXPORT_SLOTS_MAX];
    PA_LLIST_HEAD(struct memexport_slot, free_slots);
    PA_LLIST_HEAD(struct memexport_slot, used_slots);
    unsigned n_init;

    /* Called whenever a client from which we imported a memory block
       which we in turn exported to another client dies and we need to
       revoke the memory block accordingly */
    pa_memexport_revoke_cb_t revoke_cb;
    void *userdata;

    PA_LLIST_FIELDS(pa_memexport);
};

struct mempool_slot {
    PA_LLIST_FIELDS(struct mempool_slot);
    /* the actual data follows immediately hereafter */
};

struct pa_mempool {
    pa_shm memory;
    size_t block_size;
    unsigned n_blocks, n_init;

    PA_LLIST_HEAD(pa_memimport, imports);
    PA_LLIST_HEAD(pa_memexport, exports);

    /* A list of free slots that may be reused */
    PA_LLIST_HEAD(struct mempool_slot, free_slots);
    PA_LLIST_HEAD(struct mempool_slot, used_slots);
    
    pa_mempool_stat stat;
};

static void segment_detach(pa_memimport_segment *seg);

static void stat_add(pa_memblock*b) {
    assert(b);
    assert(b->pool);

    b->pool->stat.n_allocated ++;
    b->pool->stat.n_accumulated ++;
    b->pool->stat.allocated_size += b->length;
    b->pool->stat.accumulated_size += b->length;

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        b->pool->stat.n_imported++;
        b->pool->stat.imported_size += b->length;
    }

    b->pool->stat.n_allocated_by_type[b->type]++;
    b->pool->stat.n_accumulated_by_type[b->type]++;
}

static void stat_remove(pa_memblock *b) {
    assert(b);
    assert(b->pool);

    assert(b->pool->stat.n_allocated > 0);
    assert(b->pool->stat.allocated_size >= b->length);
           
    b->pool->stat.n_allocated --;
    b->pool->stat.allocated_size -= b->length;

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        assert(b->pool->stat.n_imported > 0);
        assert(b->pool->stat.imported_size >= b->length);
        
        b->pool->stat.n_imported --;
        b->pool->stat.imported_size -= b->length;
    }

    b->pool->stat.n_allocated_by_type[b->type]--;
}

static pa_memblock *memblock_new_appended(pa_mempool *p, size_t length);

pa_memblock *pa_memblock_new(pa_mempool *p, size_t length) {
    pa_memblock *b;
    
    assert(p);
    assert(length > 0);
    
    if (!(b = pa_memblock_new_pool(p, length)))
        b = memblock_new_appended(p, length);

    return b;
}

static pa_memblock *memblock_new_appended(pa_mempool *p, size_t length) {
    pa_memblock *b;

    assert(p);
    assert(length > 0);

    b = pa_xmalloc(sizeof(pa_memblock) + length);
    b->type = PA_MEMBLOCK_APPENDED;
    b->read_only = 0;
    b->ref = 1;
    b->length = length;
    b->data = (uint8_t*) b + sizeof(pa_memblock);
    b->pool = p;

    stat_add(b);
    return b;
}

static struct mempool_slot* mempool_allocate_slot(pa_mempool *p) {
    struct mempool_slot *slot;
    assert(p);

    if (p->free_slots) {
        slot = p->free_slots;
        PA_LLIST_REMOVE(struct mempool_slot, p->free_slots, slot);
    } else if (p->n_init < p->n_blocks)
        slot = (struct mempool_slot*) ((uint8_t*) p->memory.ptr + (p->block_size * p->n_init++));
    else {
        pa_log_debug("Pool full");
        p->stat.n_pool_full++;
        return NULL;
    }

    PA_LLIST_PREPEND(struct mempool_slot, p->used_slots, slot);
    return slot;
}

static void* mempool_slot_data(struct mempool_slot *slot) {
    assert(slot);

    return (uint8_t*) slot + sizeof(struct mempool_slot);
}

static unsigned mempool_slot_idx(pa_mempool *p, void *ptr) {
    assert(p);
    assert((uint8_t*) ptr >= (uint8_t*) p->memory.ptr);
    assert((uint8_t*) ptr < (uint8_t*) p->memory.ptr + p->memory.size);

    return ((uint8_t*) ptr - (uint8_t*) p->memory.ptr) / p->block_size;
}

static struct mempool_slot* mempool_slot_by_ptr(pa_mempool *p, void *ptr) {
    unsigned idx;

    if ((idx = mempool_slot_idx(p, ptr)) == (unsigned) -1)
        return NULL;

    return (struct mempool_slot*) ((uint8_t*) p->memory.ptr + (idx * p->block_size));
}

pa_memblock *pa_memblock_new_pool(pa_mempool *p, size_t length) {
    pa_memblock *b = NULL;
    struct mempool_slot *slot;

    assert(p);
    assert(length > 0);

    if (p->block_size - sizeof(struct mempool_slot) >= sizeof(pa_memblock) + length) {

        if (!(slot = mempool_allocate_slot(p)))
            return NULL;
        
        b = mempool_slot_data(slot);
        b->type = PA_MEMBLOCK_POOL;
        b->data = (uint8_t*) b + sizeof(pa_memblock);
        
    } else if (p->block_size - sizeof(struct mempool_slot) >= length) {

        if (!(slot = mempool_allocate_slot(p)))
            return NULL;
        
        b = pa_xnew(pa_memblock, 1);
        b->type = PA_MEMBLOCK_POOL_EXTERNAL;
        b->data = mempool_slot_data(slot);
    } else {
        pa_log_debug("Memory block too large for pool: %u > %u", length, p->block_size - sizeof(struct mempool_slot));
        p->stat.n_too_large_for_pool++;
        return NULL;
    }

    b->length = length;
    b->read_only = 0;
    b->ref = 1;
    b->pool = p;

    stat_add(b);
    return b;
}

pa_memblock *pa_memblock_new_fixed(pa_mempool *p, void *d, size_t length, int read_only) {
    pa_memblock *b;

    assert(p);
    assert(d);
    assert(length > 0);

    b = pa_xnew(pa_memblock, 1);
    b->type = PA_MEMBLOCK_FIXED;
    b->read_only = read_only;
    b->ref = 1;
    b->length = length;
    b->data = d;
    b->pool = p;

    stat_add(b);
    return b;
}

pa_memblock *pa_memblock_new_user(pa_mempool *p, void *d, size_t length, void (*free_cb)(void *p), int read_only) {
    pa_memblock *b;

    assert(p);
    assert(d);
    assert(length > 0);
    assert(free_cb);
    
    b = pa_xnew(pa_memblock, 1);
    b->type = PA_MEMBLOCK_USER;
    b->read_only = read_only;
    b->ref = 1;
    b->length = length;
    b->data = d;
    b->per_type.user.free_cb = free_cb;
    b->pool = p;

    stat_add(b);
    return b;
}

pa_memblock* pa_memblock_ref(pa_memblock*b) {
    assert(b);
    assert(b->ref >= 1);
    
    b->ref++;
    return b;
}

void pa_memblock_unref(pa_memblock*b) {
    assert(b);
    assert(b->ref >= 1);

    if ((--(b->ref)) > 0)
        return;
    
    stat_remove(b);

    switch (b->type) {
        case PA_MEMBLOCK_USER :
            assert(b->per_type.user.free_cb);
            b->per_type.user.free_cb(b->data);

            /* Fall through */

        case PA_MEMBLOCK_FIXED:
        case PA_MEMBLOCK_APPENDED :
            pa_xfree(b);
            break;

        case PA_MEMBLOCK_IMPORTED : {
            pa_memimport_segment *segment;

            segment = b->per_type.imported.segment;
            assert(segment);
            assert(segment->import);
            
            pa_hashmap_remove(segment->import->blocks, PA_UINT32_TO_PTR(b->per_type.imported.id));
            segment->import->release_cb(segment->import, b->per_type.imported.id, segment->import->userdata);

            if (-- segment->n_blocks <= 0)
                segment_detach(segment);
            
            pa_xfree(b);
            break;
        }

        case PA_MEMBLOCK_POOL_EXTERNAL:
        case PA_MEMBLOCK_POOL: {
            struct mempool_slot *slot;

            slot = mempool_slot_by_ptr(b->pool, b->data);
            assert(slot);
            
            PA_LLIST_REMOVE(struct mempool_slot, b->pool->used_slots, slot);
            PA_LLIST_PREPEND(struct mempool_slot, b->pool->free_slots, slot);
            
            if (b->type == PA_MEMBLOCK_POOL_EXTERNAL)
                pa_xfree(b);

            break;
        }

        case PA_MEMBLOCK_TYPE_MAX:
        default:
            abort();
    }
}

static void memblock_make_local(pa_memblock *b) {
    assert(b);

    b->pool->stat.n_allocated_by_type[b->type]--;

    if (b->length <= b->pool->block_size - sizeof(struct mempool_slot)) {
        struct mempool_slot *slot;

        if ((slot = mempool_allocate_slot(b->pool))) {
            void *new_data;
            /* We can move it into a local pool, perfect! */
            
            b->type = PA_MEMBLOCK_POOL_EXTERNAL;
            b->read_only = 0;

            new_data = mempool_slot_data(slot);
            memcpy(new_data, b->data, b->length);
            b->data = new_data;
            goto finish;
        }
    }

    /* Humm, not enough space in the pool, so lets allocate the memory with malloc() */
    b->type = PA_MEMBLOCK_USER;
    b->per_type.user.free_cb = pa_xfree;
    b->read_only = 0;
    b->data = pa_xmemdup(b->data, b->length);

finish:
    b->pool->stat.n_allocated_by_type[b->type]++;
    b->pool->stat.n_accumulated_by_type[b->type]++;
}

void pa_memblock_unref_fixed(pa_memblock *b) {
    assert(b);
    assert(b->ref >= 1);
    assert(b->type == PA_MEMBLOCK_FIXED);

    if (b->ref > 1)
        memblock_make_local(b);

    pa_memblock_unref(b);
}

static void memblock_replace_import(pa_memblock *b) {
    pa_memimport_segment *seg;
    
    assert(b);
    assert(b->type == PA_MEMBLOCK_IMPORTED);

    assert(b->pool->stat.n_imported > 0);
    assert(b->pool->stat.imported_size >= b->length);
    b->pool->stat.n_imported --;
    b->pool->stat.imported_size -= b->length;

    seg = b->per_type.imported.segment;
    assert(seg);
    assert(seg->import);

    pa_hashmap_remove(
            seg->import->blocks,
            PA_UINT32_TO_PTR(b->per_type.imported.id));

    memblock_make_local(b);

    if (-- seg->n_blocks <= 0)
        segment_detach(seg);
}

pa_mempool* pa_mempool_new(int shared) {
    size_t ps;
    pa_mempool *p;

    p = pa_xnew(pa_mempool, 1);

#ifdef HAVE_SYSCONF
    ps = (size_t) sysconf(_SC_PAGESIZE);
#elif defined(PAGE_SIZE)
	ps = (size_t) PAGE_SIZE;
#else
	ps = 4096; /* Let's hope it's like x86. */
#endif

    p->block_size = (PA_MEMPOOL_SLOT_SIZE/ps)*ps;

    if (p->block_size < ps)
        p->block_size = ps;
    
    p->n_blocks = PA_MEMPOOL_SLOTS_MAX;

    assert(p->block_size > sizeof(struct mempool_slot));

    if (pa_shm_create_rw(&p->memory, p->n_blocks * p->block_size, shared, 0700) < 0) {
        pa_xfree(p);
        return NULL;
    }

    p->n_init = 0;
    
    PA_LLIST_HEAD_INIT(pa_memimport, p->imports);
    PA_LLIST_HEAD_INIT(pa_memexport, p->exports);
    PA_LLIST_HEAD_INIT(struct mempool_slot, p->free_slots);
    PA_LLIST_HEAD_INIT(struct mempool_slot, p->used_slots);

    memset(&p->stat, 0, sizeof(p->stat));

    return p;
}

void pa_mempool_free(pa_mempool *p) {
    assert(p);

    while (p->imports)
        pa_memimport_free(p->imports);

    while (p->exports)
        pa_memexport_free(p->exports);

    if (p->stat.n_allocated > 0)
        pa_log_warn("WARNING! Memory pool destroyed but not all memory blocks freed!");
    
    pa_shm_free(&p->memory);
    pa_xfree(p);
}

const pa_mempool_stat* pa_mempool_get_stat(pa_mempool *p) {
    assert(p);

    return &p->stat;
}

void pa_mempool_vacuum(pa_mempool *p) {
    struct mempool_slot *slot;
    
    assert(p);

    for (slot = p->free_slots; slot; slot = slot->next) {
        pa_shm_punch(&p->memory, (uint8_t*) slot + sizeof(struct mempool_slot) - (uint8_t*) p->memory.ptr, p->block_size - sizeof(struct mempool_slot));
    }
}

int pa_mempool_get_shm_id(pa_mempool *p, uint32_t *id) {
    assert(p);

    if (!p->memory.shared)
        return -1;

    *id = p->memory.id;
    
    return 0;
}

int pa_mempool_is_shared(pa_mempool *p) {
    assert(p);

    return !!p->memory.shared;
}

/* For recieving blocks from other nodes */
pa_memimport* pa_memimport_new(pa_mempool *p, pa_memimport_release_cb_t cb, void *userdata) {
    pa_memimport *i;

    assert(p);
    assert(cb);
    
    i = pa_xnew(pa_memimport, 1);
    i->pool = p;
    i->segments = pa_hashmap_new(NULL, NULL);
    i->blocks = pa_hashmap_new(NULL, NULL);
    i->release_cb = cb;
    i->userdata = userdata;
    
    PA_LLIST_PREPEND(pa_memimport, p->imports, i);
    return i;
}

static void memexport_revoke_blocks(pa_memexport *e, pa_memimport *i);

static pa_memimport_segment* segment_attach(pa_memimport *i, uint32_t shm_id) {
    pa_memimport_segment* seg;

    if (pa_hashmap_size(i->segments) >= PA_MEMIMPORT_SEGMENTS_MAX)
        return NULL;

    seg = pa_xnew(pa_memimport_segment, 1);
    
    if (pa_shm_attach_ro(&seg->memory, shm_id) < 0) {
        pa_xfree(seg);
        return NULL;
    }

    seg->import = i;
    seg->n_blocks = 0;
    
    pa_hashmap_put(i->segments, PA_UINT32_TO_PTR(shm_id), seg);
    return seg;
}

static void segment_detach(pa_memimport_segment *seg) {
    assert(seg);

    pa_hashmap_remove(seg->import->segments, PA_UINT32_TO_PTR(seg->memory.id));
    pa_shm_free(&seg->memory);
    pa_xfree(seg);
}

void pa_memimport_free(pa_memimport *i) {
    pa_memexport *e;
    pa_memblock *b;
    
    assert(i);

    /* If we've exported this block further we need to revoke that export */
    for (e = i->pool->exports; e; e = e->next)
        memexport_revoke_blocks(e, i);

    while ((b = pa_hashmap_get_first(i->blocks)))
        memblock_replace_import(b);

    assert(pa_hashmap_size(i->segments) == 0);

    pa_hashmap_free(i->blocks, NULL, NULL);
    pa_hashmap_free(i->segments, NULL, NULL);
    
    PA_LLIST_REMOVE(pa_memimport, i->pool->imports, i);
    pa_xfree(i);
}

pa_memblock* pa_memimport_get(pa_memimport *i, uint32_t block_id, uint32_t shm_id, size_t offset, size_t size) {
    pa_memblock *b;
    pa_memimport_segment *seg;
    
    assert(i);

    if (pa_hashmap_size(i->blocks) >= PA_MEMIMPORT_SLOTS_MAX)
        return NULL;

    if (!(seg = pa_hashmap_get(i->segments, PA_UINT32_TO_PTR(shm_id)))) 
        if (!(seg = segment_attach(i, shm_id)))
            return NULL;

    if (offset+size > seg->memory.size)
        return NULL;
    
    b = pa_xnew(pa_memblock, 1);
    b->type = PA_MEMBLOCK_IMPORTED;
    b->read_only = 1;
    b->ref = 1;
    b->length = size;
    b->data = (uint8_t*) seg->memory.ptr + offset;
    b->pool = i->pool;
    b->per_type.imported.id = block_id;
    b->per_type.imported.segment = seg;

    pa_hashmap_put(i->blocks, PA_UINT32_TO_PTR(block_id), b);

    seg->n_blocks++;
    
    stat_add(b);
    
    return b;
}

int pa_memimport_process_revoke(pa_memimport *i, uint32_t id) {
    pa_memblock *b;
    assert(i);

    if (!(b = pa_hashmap_get(i->blocks, PA_UINT32_TO_PTR(id))))
        return -1;
    
    memblock_replace_import(b);
    return 0;
}

/* For sending blocks to other nodes */
pa_memexport* pa_memexport_new(pa_mempool *p, pa_memexport_revoke_cb_t cb, void *userdata) {
    pa_memexport *e;
    
    assert(p);
    assert(cb);

    if (!p->memory.shared)
        return NULL;
    
    e = pa_xnew(pa_memexport, 1);
    e->pool = p;
    PA_LLIST_HEAD_INIT(struct memexport_slot, e->free_slots);
    PA_LLIST_HEAD_INIT(struct memexport_slot, e->used_slots);
    e->n_init = 0;
    e->revoke_cb = cb;
    e->userdata = userdata;
    
    PA_LLIST_PREPEND(pa_memexport, p->exports, e);
    return e;
}

void pa_memexport_free(pa_memexport *e) {
    assert(e);

    while (e->used_slots)
        pa_memexport_process_release(e, e->used_slots - e->slots);

    PA_LLIST_REMOVE(pa_memexport, e->pool->exports, e);
    pa_xfree(e);
}

int pa_memexport_process_release(pa_memexport *e, uint32_t id) {
    assert(e);

    if (id >= e->n_init)
        return -1;

    if (!e->slots[id].block)
        return -1;

/*     pa_log("Processing release for %u", id); */

    assert(e->pool->stat.n_exported > 0);
    assert(e->pool->stat.exported_size >= e->slots[id].block->length);
    
    e->pool->stat.n_exported --;
    e->pool->stat.exported_size -= e->slots[id].block->length;
    
    pa_memblock_unref(e->slots[id].block);
    e->slots[id].block = NULL;

    PA_LLIST_REMOVE(struct memexport_slot, e->used_slots, &e->slots[id]);
    PA_LLIST_PREPEND(struct memexport_slot, e->free_slots, &e->slots[id]);

    return 0;
}

static void memexport_revoke_blocks(pa_memexport *e, pa_memimport *i) {
    struct memexport_slot *slot, *next;
    assert(e);
    assert(i);

    for (slot = e->used_slots; slot; slot = next) {
        uint32_t idx;
        next = slot->next;
        
        if (slot->block->type != PA_MEMBLOCK_IMPORTED ||
            slot->block->per_type.imported.segment->import != i)
            continue;

        idx = slot - e->slots;
        e->revoke_cb(e, idx, e->userdata);
        pa_memexport_process_release(e, idx);
    }
}

static pa_memblock *memblock_shared_copy(pa_mempool *p, pa_memblock *b) {
    pa_memblock *n;

    assert(p);
    assert(b);
    
    if (b->type == PA_MEMBLOCK_IMPORTED ||
        b->type == PA_MEMBLOCK_POOL ||
        b->type == PA_MEMBLOCK_POOL_EXTERNAL) {
        assert(b->pool == p);
        return pa_memblock_ref(b);
    }

    if (!(n = pa_memblock_new_pool(p, b->length)))
        return NULL;

    memcpy(n->data, b->data, b->length);
    return n;
}

int pa_memexport_put(pa_memexport *e, pa_memblock *b, uint32_t *block_id, uint32_t *shm_id, size_t *offset, size_t * size) {
    pa_shm *memory;
    struct memexport_slot *slot;
    
    assert(e);
    assert(b);
    assert(block_id);
    assert(shm_id);
    assert(offset);
    assert(size);
    assert(b->pool == e->pool);

    if (!(b = memblock_shared_copy(e->pool, b)))
        return -1;

    if (e->free_slots) {
        slot = e->free_slots;
        PA_LLIST_REMOVE(struct memexport_slot, e->free_slots, slot);
    } else if (e->n_init < PA_MEMEXPORT_SLOTS_MAX) {
        slot = &e->slots[e->n_init++];
    } else {
        pa_memblock_unref(b);
        return -1;
    }

    PA_LLIST_PREPEND(struct memexport_slot, e->used_slots, slot);
    slot->block = b;
    *block_id = slot - e->slots;

/*     pa_log("Got block id %u", *block_id); */

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        assert(b->per_type.imported.segment);
        memory = &b->per_type.imported.segment->memory;
    } else {
        assert(b->type == PA_MEMBLOCK_POOL || b->type == PA_MEMBLOCK_POOL_EXTERNAL);
        assert(b->pool);
        memory = &b->pool->memory;
    }
        
    assert(b->data >= memory->ptr);
    assert((uint8_t*) b->data + b->length <= (uint8_t*) memory->ptr + memory->size);
    
    *shm_id = memory->id;
    *offset = (uint8_t*) b->data - (uint8_t*) memory->ptr;
    *size = b->length;

    e->pool->stat.n_exported ++;
    e->pool->stat.exported_size += b->length;

    return 0;
}
