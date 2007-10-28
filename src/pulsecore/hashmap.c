/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/idxset.h>
#include <pulsecore/log.h>
#include <pulsecore/flist.h>
#include <pulsecore/macro.h>

#include "hashmap.h"

#define BUCKETS 127

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
    pa_hash_func_t hash_func;
    pa_compare_func_t compare_func;
};

PA_STATIC_FLIST_DECLARE(entries, 0, pa_xfree);

pa_hashmap *pa_hashmap_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func) {
    pa_hashmap *h;

    h = pa_xnew(pa_hashmap, 1);
    h->data = pa_xnew0(struct hashmap_entry*, h->size = BUCKETS);
    h->first_entry = NULL;
    h->n_entries = 0;
    h->hash_func = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    h->compare_func = compare_func ? compare_func : pa_idxset_trivial_compare_func;

    return h;
}

static void remove(pa_hashmap *h, struct hashmap_entry *e) {
    pa_assert(h);
    pa_assert(e);

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
        pa_assert(e->hash < h->size);
        h->data[e->hash] = e->bucket_next;
    }

    if (pa_flist_push(PA_STATIC_FLIST_GET(entries), e) < 0)
        pa_xfree(e);

    h->n_entries--;
}

void pa_hashmap_free(pa_hashmap*h, void (*free_func)(void *p, void *userdata), void *userdata) {
    pa_assert(h);

    while (h->first_entry) {
        if (free_func)
            free_func(h->first_entry->value, userdata);
        remove(h, h->first_entry);
    }

    pa_xfree(h->data);
    pa_xfree(h);
}

static struct hashmap_entry *get(pa_hashmap *h, unsigned hash, const void *key) {
    struct hashmap_entry *e;
    pa_assert(h);
    pa_assert(hash < h->size);

    for (e = h->data[hash]; e; e = e->bucket_next)
        if (h->compare_func(e->key, key) == 0)
            return e;

    return NULL;
}

int pa_hashmap_put(pa_hashmap *h, const void *key, void *value) {
    struct hashmap_entry *e;
    unsigned hash;
    pa_assert(h);

    hash = h->hash_func(key) % h->size;

    if ((e = get(h, hash, key)))
        return -1;

    if (!(e = pa_flist_pop(PA_STATIC_FLIST_GET(entries))))
        e = pa_xnew(struct hashmap_entry, 1);

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

void* pa_hashmap_get(pa_hashmap *h, const void *key) {
    unsigned hash;
    struct hashmap_entry *e;

    pa_assert(h);

    hash = h->hash_func(key) % h->size;

    if (!(e = get(h, hash, key)))
        return NULL;

    return e->value;
}

void* pa_hashmap_remove(pa_hashmap *h, const void *key) {
    struct hashmap_entry *e;
    unsigned hash;
    void *data;

    pa_assert(h);

    hash = h->hash_func(key) % h->size;

    if (!(e = get(h, hash, key)))
        return NULL;

    data = e->value;
    remove(h, e);
    return data;
}

unsigned pa_hashmap_size(pa_hashmap *h) {
    return h->n_entries;
}

void *pa_hashmap_iterate(pa_hashmap *h, void **state, const void **key) {
    pa_assert(h);
    pa_assert(state);

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

void* pa_hashmap_steal_first(pa_hashmap *h) {
    void *data;

    pa_assert(h);

    if (!h->first_entry)
        return NULL;

    data = h->first_entry->value;
    remove(h, h->first_entry);
    return data;
}

void *pa_hashmap_get_first(pa_hashmap *h) {
    pa_assert(h);

    if (!h->first_entry)
        return NULL;

    return h->first_entry->value;
}
