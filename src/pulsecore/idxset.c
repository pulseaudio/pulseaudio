/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>
#include <pulsecore/flist.h>

#include "idxset.h"

struct idxset_entry {
    void *data;
    uint32_t index;
    unsigned hash_value;

    struct idxset_entry *hash_prev, *hash_next;
    struct idxset_entry* iterate_prev, *iterate_next;
};

struct pa_idxset {
    pa_hash_func_t hash_func;
    pa_compare_func_t compare_func;

    unsigned hash_table_size, n_entries;
    struct idxset_entry **hash_table, **array, *iterate_list_head, *iterate_list_tail;
    uint32_t index, start_index, array_size;
};

PA_STATIC_FLIST_DECLARE(entries, 0, pa_xfree);

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
    return PA_PTR_TO_UINT(p);
}

int pa_idxset_trivial_compare_func(const void *a, const void *b) {
    return a != b;
}

pa_idxset* pa_idxset_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func) {
    pa_idxset *s;

    s = pa_xnew(pa_idxset, 1);
    s->hash_func = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    s->compare_func = compare_func ? compare_func : pa_idxset_trivial_compare_func;
    s->hash_table_size = 127;
    s->hash_table = pa_xnew0(struct idxset_entry*, s->hash_table_size);
    s->array = NULL;
    s->array_size = 0;
    s->index = 0;
    s->start_index = 0;
    s->n_entries = 0;

    s->iterate_list_head = s->iterate_list_tail = NULL;

    return s;
}

void pa_idxset_free(pa_idxset *s, void (*free_func) (void *p, void *userdata), void *userdata) {
    pa_assert(s);

    while (s->iterate_list_head) {
        struct idxset_entry *e = s->iterate_list_head;
        s->iterate_list_head = s->iterate_list_head->iterate_next;

        if (free_func)
            free_func(e->data, userdata);

        if (pa_flist_push(PA_STATIC_FLIST_GET(entries), e) < 0)
            pa_xfree(e);
    }

    pa_xfree(s->hash_table);
    pa_xfree(s->array);
    pa_xfree(s);
}

static struct idxset_entry* hash_scan(pa_idxset *s, struct idxset_entry* e, const void *p) {
    pa_assert(p);

    pa_assert(s->compare_func);
    for (; e; e = e->hash_next)
        if (s->compare_func(e->data, p) == 0)
            return e;

    return NULL;
}

static void extend_array(pa_idxset *s, uint32_t idx) {
    uint32_t i, j, l;
    struct idxset_entry** n;

    pa_assert(s);
    pa_assert(idx >= s->start_index);

    if (idx < s->start_index + s->array_size)
        return;

    for (i = 0; i < s->array_size; i++)
        if (s->array[i])
            break;

    l = idx - s->start_index - i + 100;
    n = pa_xnew0(struct idxset_entry*, l);

    for (j = 0; j < s->array_size-i; j++)
        n[j] = s->array[i+j];

    pa_xfree(s->array);

    s->array = n;
    s->array_size = l;
    s->start_index += i;
}

static struct idxset_entry** array_index(pa_idxset*s, uint32_t idx) {
    pa_assert(s);

    if (idx >= s->start_index + s->array_size)
        return NULL;

    if (idx < s->start_index)
        return NULL;

    return s->array + idx - s->start_index;
}

int pa_idxset_put(pa_idxset*s, void *p, uint32_t *idx) {
    unsigned h;
    struct idxset_entry *e, **a;

    pa_assert(s);
    pa_assert(p);

    pa_assert(s->hash_func);
    h = s->hash_func(p) % s->hash_table_size;

    pa_assert(s->hash_table);
    if ((e = hash_scan(s, s->hash_table[h], p))) {
        if (idx)
            *idx = e->index;

        return -1;
    }

    if (!(e = pa_flist_pop(PA_STATIC_FLIST_GET(entries))))
        e = pa_xnew(struct idxset_entry, 1);
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
    pa_assert(a && !*a);
    *a = e;

    /* Insert into linked list */
    e->iterate_next = NULL;
    e->iterate_prev = s->iterate_list_tail;
    if (s->iterate_list_tail) {
        pa_assert(s->iterate_list_head);
        s->iterate_list_tail->iterate_next = e;
    } else {
        pa_assert(!s->iterate_list_head);
        s->iterate_list_head = e;
    }
    s->iterate_list_tail = e;

    s->n_entries++;
    pa_assert(s->n_entries >= 1);

    if (idx)
        *idx = e->index;

    return 0;
}

void* pa_idxset_get_by_index(pa_idxset*s, uint32_t idx) {
    struct idxset_entry **a;
    pa_assert(s);

    if (!(a = array_index(s, idx)))
        return NULL;

    if (!*a)
        return NULL;

    return (*a)->data;
}

void* pa_idxset_get_by_data(pa_idxset*s, const void *p, uint32_t *idx) {
    unsigned h;
    struct idxset_entry *e;

    pa_assert(s);
    pa_assert(p);

    pa_assert(s->hash_func);
    h = s->hash_func(p) % s->hash_table_size;

    pa_assert(s->hash_table);
    if (!(e = hash_scan(s, s->hash_table[h], p)))
        return NULL;

    if (idx)
        *idx = e->index;

    return e->data;
}

static void remove_entry(pa_idxset *s, struct idxset_entry *e) {
    struct idxset_entry **a;

    pa_assert(s);
    pa_assert(e);

    /* Remove from array */
    a = array_index(s, e->index);
    pa_assert(a && *a && *a == e);
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

    if (pa_flist_push(PA_STATIC_FLIST_GET(entries), e) < 0)
        pa_xfree(e);

    pa_assert(s->n_entries >= 1);
    s->n_entries--;
}

void* pa_idxset_remove_by_index(pa_idxset*s, uint32_t idx) {
    struct idxset_entry **a;
    void *data;

    pa_assert(s);

    if (!(a = array_index(s, idx)))
        return NULL;

    if (!*a)
        return NULL;

    data = (*a)->data;
    remove_entry(s, *a);

    return data;
}

void* pa_idxset_remove_by_data(pa_idxset*s, const void *data, uint32_t *idx) {
    struct idxset_entry *e;
    unsigned h;
    void *r;

    pa_assert(s);

    pa_assert(s->hash_func);
    h = s->hash_func(data) % s->hash_table_size;

    pa_assert(s->hash_table);
    if (!(e = hash_scan(s, s->hash_table[h], data)))
        return NULL;

    r = e->data;
    if (idx)
        *idx = e->index;

    remove_entry(s, e);

    return r;
}

void* pa_idxset_rrobin(pa_idxset *s, uint32_t *idx) {
    struct idxset_entry **a, *e = NULL;

    pa_assert(s);
    pa_assert(idx);

    if ((a = array_index(s, *idx)) && *a)
        e = (*a)->iterate_next;

    if (!e)
        e = s->iterate_list_head;

    if (!e)
        return NULL;

    *idx = e->index;
    return e->data;
}

void* pa_idxset_first(pa_idxset *s, uint32_t *idx) {
    pa_assert(s);

    if (!s->iterate_list_head)
        return NULL;

    if (idx)
        *idx = s->iterate_list_head->index;
    return s->iterate_list_head->data;
}

void *pa_idxset_next(pa_idxset *s, uint32_t *idx) {
    struct idxset_entry **a, *e = NULL;

    pa_assert(s);
    pa_assert(idx);

    if ((a = array_index(s, *idx)) && *a)
        e = (*a)->iterate_next;

    if (e) {
        *idx = e->index;
        return e->data;
    } else {
        *idx = PA_IDXSET_INVALID;
        return NULL;
    }
}

int pa_idxset_foreach(pa_idxset*s, int (*func)(void *p, uint32_t idx, int *del, void*userdata), void *userdata) {
    struct idxset_entry *e;

    pa_assert(s);
    pa_assert(func);

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

unsigned pa_idxset_size(pa_idxset*s) {
    pa_assert(s);

    return s->n_entries;
}

int pa_idxset_isempty(pa_idxset *s) {
    pa_assert(s);

    return s->n_entries == 0;
}

