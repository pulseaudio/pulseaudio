#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "idxset.h"

struct idxset_entry {
    void *data;
    uint32_t index;
    unsigned hash_value;

    struct idxset_entry *hash_prev, *hash_next;
    struct idxset_entry* iterate_prev, *iterate_next;
};

struct pa_idxset {
    unsigned (*hash_func) (const void *p);
    int (*compare_func)(const void *a, const void *b);
    
    unsigned hash_table_size, n_entries;
    struct idxset_entry **hash_table, **array, *iterate_list_head, *iterate_list_tail;
    uint32_t index, start_index, array_size;
};

unsigned pa_idxset_string_hash_func(const void *p) {
    unsigned hash = 0;
    const char *c;
    
    for (c = p; *c; c++)
        hash = 31 * hash + *c;

    return hash;
}

int pa_idxset_string_compare_func(const void *a, const void *b) {
    return strcmp(a, b);
}

unsigned pa_idxset_trivial_hash_func(const void *p) {
    return (unsigned) p;
}

int pa_idxset_trivial_compare_func(const void *a, const void *b) {
    return a != b;
}

struct pa_idxset* pa_idxset_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b)) {
    struct pa_idxset *s;

    s = malloc(sizeof(struct pa_idxset));
    assert(s);
    s->hash_func = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    s->compare_func = compare_func ? compare_func : pa_idxset_trivial_compare_func;
    s->hash_table_size = 1023;
    s->hash_table = malloc(sizeof(struct idxset_entry*)*s->hash_table_size);
    assert(s->hash_table);
    memset(s->hash_table, 0, sizeof(struct idxset_entry*)*s->hash_table_size);
    s->array = NULL;
    s->array_size = 0;
    s->index = 0;
    s->start_index = 0;
    s->n_entries = 0;

    s->iterate_list_head = s->iterate_list_tail = NULL;

    return s;
}

void pa_idxset_free(struct pa_idxset *s, void (*free_func) (void *p, void *userdata), void *userdata) {
    assert(s);

    if (free_func) {
        while (s->iterate_list_head) {
            struct idxset_entry *e = s->iterate_list_head;
            s->iterate_list_head = s->iterate_list_head->iterate_next;

            if (free_func)
                free_func(e->data, userdata);
            free(e);
        }
    }

    free(s->hash_table);
    free(s->array);
    free(s);
}

static struct idxset_entry* hash_scan(struct pa_idxset *s, struct idxset_entry* e, void *p) {
    assert(p);

    assert(s->compare_func);
    for (; e; e = e->hash_next)
        if (s->compare_func(e->data, p) == 0)
            return e;

    return NULL;
}

static void extend_array(struct pa_idxset *s, uint32_t index) {
    uint32_t i, j, l;
    struct idxset_entry** n;
    assert(index >= s->start_index);

    if (index < s->start_index + s->array_size)
        return;

    for (i = 0; i < s->array_size; i++)
        if (s->array[i])
            break;

    l = index - s->start_index - i + 100;
    n = malloc(sizeof(struct hash_table_entry*)*l);
    assert(n);
    memset(n, 0, sizeof(struct hash_table_entry*)*l);
    
    for (j = 0; j < s->array_size-i; j++)
        n[j] = s->array[i+j];

    free(s->array);
    
    s->array = n;
    s->array_size = l;
    s->start_index += i;
}

static struct idxset_entry** array_index(struct pa_idxset*s, uint32_t index) {
    if (index >= s->start_index + s->array_size)
        return NULL;
    
    if (index < s->start_index)
        return NULL;
    
    return s->array + (index - s->start_index);
}

int pa_idxset_put(struct pa_idxset*s, void *p, uint32_t *index) {
    unsigned h;
    struct idxset_entry *e, **a;
    assert(s && p);

    assert(s->hash_func);
    h = s->hash_func(p) % s->hash_table_size;

    assert(s->hash_table);
    if ((e = hash_scan(s, s->hash_table[h], p))) {
        if (index)
            *index = e->index;
        
        return -1;
    }

    e = malloc(sizeof(struct idxset_entry));
    assert(e);

    e->data = p;
    e->index = s->index++;
    e->hash_value = h;

    /* Insert into hash table */
    e->hash_next = s->hash_table[h];
    e->hash_prev = NULL;
    if (s->hash_table[h])
        s->hash_table[h]->hash_prev = e;
    s->hash_table[h] = e;

    /* Insert into array */
    extend_array(s, e->index);
    a = array_index(s, e->index);
    assert(a && !*a);
    *a = e;

    /* Insert into linked list */
    e->iterate_next = NULL;
    e->iterate_prev = s->iterate_list_tail;
    if (s->iterate_list_tail) {
        assert(s->iterate_list_head);
        s->iterate_list_tail->iterate_next = e;
    } else {
        assert(!s->iterate_list_head);
        s->iterate_list_head = e;
    }
    s->iterate_list_tail = e;
    
    s->n_entries++;
    assert(s->n_entries >= 1);
    
    if (index)
        *index = e->index;

    return 0;
}

void* pa_idxset_get_by_index(struct pa_idxset*s, uint32_t index) {
    struct idxset_entry **a;
    assert(s);
    
    if (!(a = array_index(s, index)))
        return NULL;

    if (!*a)
        return NULL;

    return (*a)->data;
}

void* pa_idxset_get_by_data(struct pa_idxset*s, void *p, uint32_t *index) {
    unsigned h;
    struct idxset_entry *e;
    assert(s && p);
    
    assert(s->hash_func);
    h = s->hash_func(p) % s->hash_table_size;

    assert(s->hash_table);
    if (!(e = hash_scan(s, s->hash_table[h], p)))
        return NULL;

    if (index)
        *index = e->index;

    return e->data;
}

static void remove_entry(struct pa_idxset *s, struct idxset_entry *e) {
    struct idxset_entry **a;
    assert(s && e);

    /* Remove from array */
    a = array_index(s, e->index);
    assert(a && *a && *a == e);
    *a = NULL;
    
    /* Remove from linked list */
    if (e->iterate_next)
        e->iterate_next->iterate_prev = e->iterate_prev;
    else
        s->iterate_list_tail = e->iterate_prev;
    
    if (e->iterate_prev)
        e->iterate_prev->iterate_next = e->iterate_next;
    else
        s->iterate_list_head = e->iterate_next;

    /* Remove from hash table */
    if (e->hash_next)
        e->hash_next->hash_prev = e->hash_prev;

    if (e->hash_prev)
        e->hash_prev->hash_next = e->hash_next;
    else
        s->hash_table[e->hash_value] = e->hash_next;

    free(e);

    assert(s->n_entries >= 1);
    s->n_entries--;
}

void* pa_idxset_remove_by_index(struct pa_idxset*s, uint32_t index) {
    struct idxset_entry **a;
    void *data;
    
    assert(s);

    if (!(a = array_index(s, index)))
        return NULL;

    data = (*a)->data;
    remove_entry(s, *a);
    
    return data; 
}

void* pa_idxset_remove_by_data(struct pa_idxset*s, void *data, uint32_t *index) {
    struct idxset_entry *e;
    unsigned h;
    
    assert(s->hash_func);
    h = s->hash_func(data) % s->hash_table_size;

    assert(s->hash_table);
    if (!(e = hash_scan(s, s->hash_table[h], data)))
        return NULL;

    data = e->data;
    if (index)
        *index = e->index;

    remove_entry(s, e);

    return data;
}

void* pa_idxset_rrobin(struct pa_idxset *s, uint32_t *index) {
    struct idxset_entry **a, *e = NULL;
    assert(s && index);

    if ((a = array_index(s, *index)) && *a)
        e = (*a)->iterate_next;

    if (!e)
        e = s->iterate_list_head;

    if (!e)
        return NULL;
    
    *index = e->index;
    return e->data;
}

void* pa_idxset_first(struct pa_idxset *s, uint32_t *index) {
    assert(s);

    if (!s->iterate_list_head)
        return NULL;

    if (index)
        *index = s->iterate_list_head->index;
    return s->iterate_list_head->data;
}

void *pa_idxset_next(struct pa_idxset *s, uint32_t *index) {
    struct idxset_entry **a, *e = NULL;
    assert(s && index);

    if ((a = array_index(s, *index)) && *a)
        e = (*a)->iterate_next;
    
    if (e) {
        *index = e->index;
        return e->data;
    } else {
        *index = PA_IDXSET_INVALID;
        return NULL;
    }
}


int pa_idxset_foreach(struct pa_idxset*s, int (*func)(void *p, uint32_t index, int *del, void*userdata), void *userdata) {
    struct idxset_entry *e;
    assert(s && func);

    e = s->iterate_list_head;
    while (e) {
        int del = 0, r;
        struct idxset_entry *n = e->iterate_next;

        r = func(e->data, e->index, &del, userdata);

        if (del)
            remove_entry(s, e);

        if (r < 0)
            return r;

        e = n;
    }
    
    return 0;
}

unsigned pa_idxset_ncontents(struct pa_idxset*s) {
    assert(s);
    return s->n_entries;
}

int pa_idxset_isempty(struct pa_idxset *s) {
    assert(s);
    return s->n_entries == 0;
}

