#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "scache.h"
#include "sink-input.h"
#include "mainloop.h"
#include "sample-util.h"
#include "play-memchunk.h"

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

    e->volume = 0x100;
    
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

int pa_scache_play_item(struct pa_core *c, const char *name, struct pa_sink *sink, uint32_t volume) {
    struct pa_scache_entry *e;
    assert(c && name && sink);

    if (!c->scache_hashmap || !(e = pa_hashmap_get(c->scache_hashmap, name)))
        return -1;

    if (!e->memchunk.memblock)
        return -1;

    if (pa_play_memchunk(sink, name, &e->sample_spec, &e->memchunk, pa_volume_multiply(volume, e->volume)) < 0)
        return -1;
    
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
