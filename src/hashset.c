#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "hashset.h"
#include "idxset.h"

struct hashset_entry {
    struct hashset_entry *next, *previous, *bucket_next, *bucket_previous;
    unsigned hash;
    const void *key;
    void *value;
};

struct pa_hashset {
    unsigned size;
    struct hashset_entry **data;
    struct hashset_entry *first_entry;
    
    unsigned n_entries;
    unsigned (*hash_func) (const void *p);
    int (*compare_func) (const void*a, const void*b);
};

struct pa_hashset *pa_hashset_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b)) {
    struct pa_hashset *h;
    h = malloc(sizeof(struct pa_hashset));
    assert(h);
    h->data = malloc(sizeof(struct hashset_entry*)*(h->size = 1023));
    assert(h->data);
    memset(h->data, 0, sizeof(struct hashset_entry*)*(h->size = 1023));
    h->first_entry = NULL;
    h->n_entries = 0;
    h->hash_func = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    h->compare_func = compare_func ? compare_func : pa_idxset_trivial_compare_func;
    return h;
}

static void remove(struct pa_hashset *h, struct hashset_entry *e) {
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
    else
        h->data[e->hash] = e->bucket_next;

    free(e);
    h->n_entries--;
}

void pa_hashset_free(struct pa_hashset*h, void (*free_func)(void *p, void *userdata), void *userdata) {
    assert(h);

    while (h->first_entry) {
        if (free_func)
            free_func(h->first_entry->value, userdata);
        remove(h, h->first_entry);
    }
    
    free(h->data);
    free(h);
}

static struct hashset_entry *get(struct pa_hashset *h, unsigned hash, const void *key) {
    struct hashset_entry *e;

    for (e = h->data[hash]; e; e = e->bucket_next)
        if (h->compare_func(e->key, key) == 0)
            return e;

    return NULL;
}

int pa_hashset_put(struct pa_hashset *h, const void *key, void *value) {
    struct hashset_entry *e;
    unsigned hash;
    assert(h && key);

    hash = h->hash_func(key) % h->size;

    if ((e = get(h, hash, key)))
        return -1;
    
    e = malloc(sizeof(struct hashset_entry));
    assert(e);
    
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

void* pa_hashset_get(struct pa_hashset *h, const void *key) {
    unsigned hash;
    struct hashset_entry *e;
    assert(h && key);

    hash = h->hash_func(key) % h->size;

    if (!(e = get(h, hash, key)))
        return NULL;

    return e->value;
}

int pa_hashset_remove(struct pa_hashset *h, const void *key) {
    struct hashset_entry *e;
    unsigned hash;
    assert(h && key);

    hash = h->hash_func(key) % h->size;

    if (!(e = get(h, hash, key)))
        return 1;

    remove(h, e);
    return 0;
}

unsigned pa_hashset_ncontents(struct pa_hashset *h) {
    return h->n_entries;
}
