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

#include <assert.h>

#include "xmalloc.h"
#include "props.h"
#include "log.h"

struct pa_property {
    char *name;  /* Points to memory allocated by the property subsystem */
    void *data;  /* Points to memory maintained by the caller */
};

/* Allocate a new property object */
static struct pa_property* property_new(const char *name, void *data) {
    struct pa_property* p;
    assert(name && data);
    
    p = pa_xmalloc(sizeof(struct pa_property));
    p->name = pa_xstrdup(name);
    p->data = data;

    return p;
}

/* Free a property object */
static void property_free(struct pa_property *p) {
    assert(p);

    pa_xfree(p->name);
    pa_xfree(p);
}

void* pa_property_get(struct pa_core *c, const char *name) {
    struct pa_property *p;
    assert(c && name && c->properties);

    if (!(p = pa_hashmap_get(c->properties, name)))
        return NULL;

    return p->data;
}

int pa_property_set(struct pa_core *c, const char *name, void *data) {
    struct pa_property *p;
    assert(c && name && data && c->properties);

    if (pa_hashmap_get(c->properties, name))
        return -1;

    p = property_new(name, data);
    pa_hashmap_put(c->properties, p->name, p);
    return 0;
}

int pa_property_remove(struct pa_core *c, const char *name) {
    struct pa_property *p;
    assert(c && name && c->properties);

    if (!(p = pa_hashmap_remove(c->properties, name)))
        return -1;
    
    property_free(p);
    return 0;
}

void pa_property_init(struct pa_core *c) {
    assert(c);

    c->properties = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
}

void pa_property_cleanup(struct pa_core *c) {
    assert(c);

    if (!c->properties)
        return;

    assert(!pa_hashmap_ncontents(c->properties));

    pa_hashmap_free(c->properties, NULL, NULL);
    c->properties = NULL;
    
}

void pa_property_dump(struct pa_core *c, struct pa_strbuf *s) {
    void *state = NULL;
    struct pa_property *p;
    assert(c && s);

    while ((p = pa_hashmap_iterate(c->properties, &state, NULL)))
        pa_strbuf_printf(s, "[%s] -> [%p]\n", p->name, p->data);
}

int pa_property_replace(struct pa_core *c, const char *name, void *data) {
    assert(c && name);

    pa_property_remove(c, name);
    return pa_property_set(c, name, data);
}
