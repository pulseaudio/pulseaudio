#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "scache.h"
#include "sink-input.h"
#include "mainloop.h"

static void free_entry(struct pa_scache_entry *e) {
    assert(e);
    free(e->name);
    if (e->memchunk.memblock)
        pa_memblock_unref(e->memchunk.memblock);
    free(e);
}

void pa_scache_add_item(struct pa_core *c, const char *name, struct pa_sample_spec *ss, struct pa_memchunk *chunk, uint32_t *index) {
    struct pa_scache_entry *e;
    int put;
    assert(c && name);

    if (c->scache_hashmap && (e = pa_hashmap_get(c->scache_hashmap, name))) {
        put = 0;
        if (e->memchunk.memblock)
            pa_memblock_unref(e->memchunk.memblock);
    } else {
        put = 1;
        e = malloc(sizeof(struct pa_scache_entry));
        assert(e);
        e->name = strdup(name);
        assert(e->name);
    }

    if (ss)
        e->sample_spec = *ss;
    else
        memset(&e->sample_spec, 0, sizeof(struct pa_sample_spec));

    if (chunk) {
        e->memchunk = *chunk;
        pa_memblock_ref(e->memchunk.memblock);
    } else {
        e->memchunk.memblock = NULL;
        e->memchunk.index = e->memchunk.length = 0;
    }

    if (put) {
        if (!c->scache_hashmap) {
            c->scache_hashmap = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
            assert(c->scache_hashmap);
        }
        
        if (!c->scache_idxset) {
            c->scache_idxset = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
            assert(c->scache_idxset);
        }
        
        pa_idxset_put(c->scache_idxset, e, &e->index);
        pa_hashmap_put(c->scache_hashmap, e->name, e);
    }
        
    if (index)
        *index = e->index;
}

int pa_scache_remove_item(struct pa_core *c, const char *name) {
    struct pa_scache_entry *e;
    assert(c && name);

    if (!c->scache_hashmap || !(e = pa_hashmap_get(c->scache_hashmap, name)))
        return -1;

    pa_hashmap_remove(c->scache_hashmap, name);
    if (pa_idxset_remove_by_data(c->scache_idxset, e, NULL) != e)
        assert(0);
    free_entry(e);
    return 0;
}

static void free_cb(void *p, void *userdata) {
    struct pa_scache_entry *e = p;
    assert(e);
    free_entry(e);
}

void pa_scache_free(struct pa_core *c) {
    assert(c);

    if (c->scache_hashmap) {
        pa_hashmap_free(c->scache_hashmap, free_cb, NULL);
        c->scache_hashmap = NULL;
    }

    if (c->scache_idxset) {
        pa_idxset_free(c->scache_idxset, NULL, NULL);
        c->scache_idxset = NULL;
    }
}

static void sink_input_kill(struct pa_sink_input *i) {
    struct pa_memchunk *c;
    assert(i && i->userdata);
    c = i->userdata;

    pa_memblock_unref(c->memblock);
    free(c);
    pa_sink_input_free(i);
}

static int sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    struct pa_memchunk *c;
    assert(i && chunk && i->userdata);
    c = i->userdata;

    assert(c->length && c->memblock && c->memblock->length);
    *chunk = *c;
    pa_memblock_ref(c->memblock);

    return 0;
}

static void si_kill(void *i) {
    sink_input_kill(i);
}

static void sink_input_drop(struct pa_sink_input *i, size_t length) {
    struct pa_memchunk *c;
    assert(i && length && i->userdata);
    c = i->userdata;

    assert(length <= c->length);

    c->length -= length;
    c->index += length;

    if (c->length <= 0)
        pa_mainloop_api_once(i->sink->core->mainloop, si_kill, i);
}

int pa_scache_play_item(struct pa_core *c, const char *name, struct pa_sink *sink, uint32_t volume) {
    struct pa_sink_input *si;
    struct pa_scache_entry *e;
    struct pa_memchunk *chunk;
    assert(c && name && sink);

    if (!c->scache_hashmap || !(e = pa_hashmap_get(c->scache_hashmap, name)))
        return -1;

    if (!e->memchunk.memblock)
        return -1;
    
    if (!(si = pa_sink_input_new(sink, name, &e->sample_spec)))
        return -1;

    si->volume = volume;

    si->peek = sink_input_peek;
    si->drop = sink_input_drop;
    si->kill = sink_input_kill;
    si->userdata = chunk = malloc(sizeof(struct pa_memchunk));
    assert(chunk);
    *chunk = e->memchunk;
    pa_memblock_ref(chunk->memblock);

    return 0;
}

const char * pa_scache_get_name_by_id(struct pa_core *c, uint32_t id) {
    struct pa_scache_entry *e;
    assert(c && id != PA_IDXSET_INVALID);

    if (!c->scache_idxset || !(e = pa_idxset_get_by_index(c->scache_idxset, id)))
        return NULL;

    return e->name;
    
}

uint32_t pa_scache_get_id_by_name(struct pa_core *c, const char *name) {
    struct pa_scache_entry *e;
    assert(c && name);

    if (!c->scache_hashmap || !(e = pa_hashmap_get(c->scache_hashmap, name)))
        return PA_IDXSET_INVALID;

    return e->index;
}
