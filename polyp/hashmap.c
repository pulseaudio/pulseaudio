/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "hashmap.h"
#include "idxset.h"
#include "xmalloc.h"
#include "log.h"

#define BUCKETS 1023

struct hashmap_entry {
    struct hashmap_entry *next, *previous, *bucket_next, *bucket_previous;
    unsigned hash;
    const void *key;
    void *value;
};

struct pa_hashmap {
    unsigned size;
    struct hashmap_entry **data;
    struct hashmap_entry *first_entry;
    
    unsigned n_entries;
    unsigned (*hash_func) (const void *p);
    int (*compare_func) (const void*a, const void*b);
};

struct pa_hashmap *pa_hashmap_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b)) {
    struct pa_hashmap *h;
    h = pa_xmalloc(sizeof(struct pa_hashmap));
    h->data = pa_xmalloc0(sizeof(struct hashmap_entry*)*(h->size = BUCKETS));
    h->first_entry = NULL;
    h->n_entries = 0;
    h->hash_func = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    h->compare_func = compare_func ? compare_func : pa_idxset_trivial_compare_func;
    return h;
}

static void remove(struct pa_hashmap *h, struct hashmap_entry *e) {
    assert(e);

    if (e->next)
        e->next->previous = e->previous;
    if (e->previous)
        e->previous->next = e->next;
    else
        h->first_entry = e->next;

    if (e->bucket_next)
        e->bucket_next->bucket_previous = e->bucket_previous;
    if (e->bucket_previous)
        e->bucket_previous->bucket_next = e->bucket_next;
    else {
        assert(e->hash < h->size);
        h->data[e->hash] = e->bucket_next;
    }

    pa_xfree(e);
    h->n_entries--;
}

void pa_hashmap_free(struct pa_hashmap*h, void (*free_func)(void *p, void *userdata), void *userdata) {
    assert(h);

    while (h->first_entry) {
        if (free_func)
            free_func(h->first_entry->value, userdata);
        remove(h, h->first_entry);
    }
    
    pa_xfree(h->data);
    pa_xfree(h);
}

static struct hashmap_entry *get(struct pa_hashmap *h, unsigned hash, const void *key) {
    struct hashmap_entry *e;
    assert(h && hash < h->size);

    for (e = h->data[hash]; e; e = e->bucket_next)
        if (h->compare_func(e->key, key) == 0)
            return e;

    return NULL;
}

int pa_hashmap_put(struct pa_hashmap *h, const void *key, void *value) {
    struct hashmap_entry *e;
    unsigned hash;
    assert(h);

    hash = h->hash_func(key) % h->size;

    if ((e = get(h, hash, key)))
        return -1;
    
    e = pa_xmalloc(sizeof(struct hashmap_entry));
    e->hash = hash;
    e->key = key;
    e->value = value;
    
    e->previous = NULL;
    e->next = h->first_entry;
    if (h->first_entry)
        h->first_entry->previous = e;
    h->first_entry = e;
    
    e->bucket_previous = NULL;
    e->bucket_next = h->data[hash];
    if (h->data[hash])
        h->data[hash]->bucket_previous = e;
    h->data[hash] = e;
    
    h->n_entries ++;
    return 0;
}

void* pa_hashmap_get(struct pa_hashmap *h, const void *key) {
    unsigned hash;
    struct hashmap_entry *e;
    assert(h && key);

    hash = h->hash_func(key) % h->size;

    if (!(e = get(h, hash, key)))
        return NULL;

    return e->value;
}

void* pa_hashmap_remove(struct pa_hashmap *h, const void *key) {
    struct hashmap_entry *e;
    unsigned hash;
    void *data;
    assert(h && key);

    hash = h->hash_func(key) % h->size;

    if (!(e = get(h, hash, key)))
        return NULL;

    data = e->value;
    remove(h, e);
    return data;
}

unsigned pa_hashmap_ncontents(struct pa_hashmap *h) {
    return h->n_entries;
}

void *pa_hashmap_iterate(struct pa_hashmap *h, void **state, const void **key) {
    assert(h && state);

    if (!*state) 
        *state = h->first_entry;
    else
        *state = ((struct hashmap_entry*) *state)->next;

    if (!*state) {
        if (key)
            *key = NULL;
        return NULL;
    }

    if (key)
        *key = ((struct hashmap_entry*) *state)->key;
    
    return ((struct hashmap_entry*) *state)->value;
}
