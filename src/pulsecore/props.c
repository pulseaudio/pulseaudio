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

#include <pulse/xmalloc.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "props.h"

typedef struct pa_property {
    char *name;  /* Points to memory allocated by the property subsystem */
    void *data;  /* Points to memory maintained by the caller */
} pa_property;

/* Allocate a new property object */
static pa_property* property_new(const char *name, void *data) {
    pa_property* p;

    pa_assert(name);
    pa_assert(data);

    p = pa_xnew(pa_property, 1);
    p->name = pa_xstrdup(name);
    p->data = data;

    return p;
}

/* Free a property object */
static void property_free(pa_property *p) {
    pa_assert(p);

    pa_xfree(p->name);
    pa_xfree(p);
}

void* pa_property_get(pa_core *c, const char *name) {
    pa_property *p;

    pa_assert(c);
    pa_assert(name);
    pa_assert(c->properties);

    if (!(p = pa_hashmap_get(c->properties, name)))
        return NULL;

    return p->data;
}

int pa_property_set(pa_core *c, const char *name, void *data) {
    pa_property *p;

    pa_assert(c);
    pa_assert(name);
    pa_assert(data);
    pa_assert(c->properties);

    if (pa_hashmap_get(c->properties, name))
        return -1;

    p = property_new(name, data);
    pa_hashmap_put(c->properties, p->name, p);
    return 0;
}

int pa_property_remove(pa_core *c, const char *name) {
    pa_property *p;

    pa_assert(c);
    pa_assert(name);
    pa_assert(c->properties);

    if (!(p = pa_hashmap_remove(c->properties, name)))
        return -1;

    property_free(p);
    return 0;
}

void pa_property_init(pa_core *c) {
    pa_assert(c);

    c->properties = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
}

void pa_property_cleanup(pa_core *c) {
    pa_assert(c);

    if (!c->properties)
        return;

    pa_assert(!pa_hashmap_size(c->properties));

    pa_hashmap_free(c->properties, NULL, NULL);
    c->properties = NULL;

}

void pa_property_dump(pa_core *c, pa_strbuf *s) {
    void *state = NULL;
    pa_property *p;

    pa_assert(c);
    pa_assert(s);

    while ((p = pa_hashmap_iterate(c->properties, &state, NULL)))
        pa_strbuf_printf(s, "[%s] -> [%p]\n", p->name, p->data);
}

int pa_property_replace(pa_core *c, const char *name, void *data) {
    pa_assert(c);
    pa_assert(name);

    pa_property_remove(c, name);
    return pa_property_set(c, name, data);
}
