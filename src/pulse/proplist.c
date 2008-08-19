/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

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

#include <string.h>

#include <pulse/xmalloc.h>
#include <pulse/utf8.h>
#include <pulse/i18n.h>

#include <pulsecore/hashmap.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/core-util.h>

#include "proplist.h"

struct property {
    char *key;
    void *value;
    size_t nbytes;
};

#define MAKE_HASHMAP(p) ((pa_hashmap*) (p))
#define MAKE_PROPLIST(p) ((pa_proplist*) (p))

static pa_bool_t property_name_valid(const char *key) {

    if (!pa_utf8_valid(key))
        return FALSE;

    if (strlen(key) <= 0)
        return FALSE;

    return TRUE;
}

static void property_free(struct property *prop) {
    pa_assert(prop);

    pa_xfree(prop->key);
    pa_xfree(prop->value);
    pa_xfree(prop);
}

pa_proplist* pa_proplist_new(void) {
    pa_init_i18n();

    return MAKE_PROPLIST(pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func));
}

void pa_proplist_free(pa_proplist* p) {
    pa_assert(p);

    pa_proplist_clear(p);
    pa_hashmap_free(MAKE_HASHMAP(p), NULL, NULL);
}

/** Will accept only valid UTF-8 */
int pa_proplist_sets(pa_proplist *p, const char *key, const char *value) {
    struct property *prop;
    pa_bool_t add = FALSE;

    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key) || !pa_utf8_valid(value))
        return -1;

    if (!(prop = pa_hashmap_get(MAKE_HASHMAP(p), key))) {
        prop = pa_xnew(struct property, 1);
        prop->key = pa_xstrdup(key);
        add = TRUE;
    } else
        pa_xfree(prop->value);

    prop->value = pa_xstrdup(value);
    prop->nbytes = strlen(value)+1;

    if (add)
        pa_hashmap_put(MAKE_HASHMAP(p), prop->key, prop);

    return 0;
}

/** Will accept only valid UTF-8 */
int pa_proplist_setf(pa_proplist *p, const char *key, const char *format, ...) {
    va_list ap;
    int r;
    char *t;

    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key) || !pa_utf8_valid(format))
        return -1;

    va_start(ap, format);
    t = pa_vsprintf_malloc(format, ap);
    va_end(ap);

    r = pa_proplist_sets(p, key, t);

    pa_xfree(t);
    return r;
}

int pa_proplist_set(pa_proplist *p, const char *key, const void *data, size_t nbytes) {
    struct property *prop;
    pa_bool_t add = FALSE;

    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key))
        return -1;

    if (!(prop = pa_hashmap_get(MAKE_HASHMAP(p), key))) {
        prop = pa_xnew(struct property, 1);
        prop->key = pa_xstrdup(key);
        add = TRUE;
    } else
        pa_xfree(prop->value);

    prop->value = pa_xmemdup(data, nbytes);
    prop->nbytes = nbytes;

    if (add)
        pa_hashmap_put(MAKE_HASHMAP(p), prop->key, prop);

    return 0;
}

const char *pa_proplist_gets(pa_proplist *p, const char *key) {
    struct property *prop;

    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key))
        return NULL;

    if (!(prop = pa_hashmap_get(MAKE_HASHMAP(p), key)))
        return NULL;

    if (prop->nbytes <= 0)
        return NULL;

    if (((char*) prop->value)[prop->nbytes-1] != 0)
        return NULL;

    if (strlen((char*) prop->value) != prop->nbytes-1)
        return NULL;

    if (!pa_utf8_valid((char*) prop->value))
        return NULL;

    return (char*) prop->value;
}

int pa_proplist_get(pa_proplist *p, const char *key, const void **data, size_t *nbytes) {
    struct property *prop;

    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key))
        return -1;

    if (!(prop = pa_hashmap_get(MAKE_HASHMAP(p), key)))
        return -1;

    *data = prop->value;
    *nbytes = prop->nbytes;

    return 0;
}

void pa_proplist_update(pa_proplist *p, pa_update_mode_t mode, pa_proplist *other) {
    struct property *prop;
    void *state = NULL;

    pa_assert(p);
    pa_assert(mode == PA_UPDATE_SET || mode == PA_UPDATE_MERGE || mode == PA_UPDATE_REPLACE);
    pa_assert(other);

    if (mode == PA_UPDATE_SET)
        pa_proplist_clear(p);

    while ((prop = pa_hashmap_iterate(MAKE_HASHMAP(other), &state, NULL))) {

        if (mode == PA_UPDATE_MERGE && pa_proplist_contains(p, prop->key))
            continue;

        pa_assert_se(pa_proplist_set(p, prop->key, prop->value, prop->nbytes) == 0);
    }
}

int pa_proplist_unset(pa_proplist *p, const char *key) {
    struct property *prop;

    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key))
        return -1;

    if (!(prop = pa_hashmap_remove(MAKE_HASHMAP(p), key)))
        return -2;

    property_free(prop);
    return 0;
}

int pa_proplist_unset_many(pa_proplist *p, const char * const keys[]) {
    const char * const * k;
    int n = 0;

    pa_assert(p);
    pa_assert(keys);

    for (k = keys; *k; k++)
        if (!property_name_valid(*k))
            return -1;

    for (k = keys; *k; k++)
        if (pa_proplist_unset(p, *k) >= 0)
            n++;

    return n;
}

const char *pa_proplist_iterate(pa_proplist *p, void **state) {
    struct property *prop;

    if (!(prop = pa_hashmap_iterate(MAKE_HASHMAP(p), state, NULL)))
        return NULL;

    return prop->key;
}

char *pa_proplist_to_string(pa_proplist *p) {
    const char *key;
    void *state = NULL;
    pa_strbuf *buf;

    pa_assert(p);

    buf = pa_strbuf_new();

    while ((key = pa_proplist_iterate(p, &state))) {

        const char *v;

        if ((v = pa_proplist_gets(p, key)))
            pa_strbuf_printf(buf, "%s = \"%s\"\n", key, v);
        else {
            const void *value;
            size_t nbytes;
            char *c;

            pa_assert_se(pa_proplist_get(p, key, &value, &nbytes) == 0);
            c = pa_xmalloc(nbytes*2+1);
            pa_hexstr((const uint8_t*) value, nbytes, c, nbytes*2+1);

            pa_strbuf_printf(buf, "%s = hex:%s\n", key, c);
            pa_xfree(c);
        }
    }

    return pa_strbuf_tostring_free(buf);
}

int pa_proplist_contains(pa_proplist *p, const char *key) {
    pa_assert(p);
    pa_assert(key);

    if (!property_name_valid(key))
        return -1;

    if (!(pa_hashmap_get(MAKE_HASHMAP(p), key)))
        return 0;

    return 1;
}

void pa_proplist_clear(pa_proplist *p) {
    struct property *prop;
    pa_assert(p);

    while ((prop = pa_hashmap_steal_first(MAKE_HASHMAP(p))))
        property_free(prop);
}

pa_proplist* pa_proplist_copy(pa_proplist *template) {
    pa_proplist *p;

    pa_assert_se(p = pa_proplist_new());

    if (template)
        pa_proplist_update(p, PA_UPDATE_REPLACE, template);

    return p;
}
