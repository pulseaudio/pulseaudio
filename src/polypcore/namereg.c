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
#include <string.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "namereg.h"
#include "autoload.h"
#include "source.h"
#include "sink.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "util.h"

struct namereg_entry {
    pa_namereg_type_t type;
    char *name;
    void *data;
};

void pa_namereg_free(pa_core *c) {
    assert(c);
    if (!c->namereg)
        return;
    assert(pa_hashmap_size(c->namereg) == 0);
    pa_hashmap_free(c->namereg, NULL, NULL);
}

const char *pa_namereg_register(pa_core *c, const char *name, pa_namereg_type_t type, void *data, int fail) {
    struct namereg_entry *e;
    char *n = NULL;
    int r;
    
    assert(c && name && data);

    if (!c->namereg) {
        c->namereg = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        assert(c->namereg);
    }

    if ((e = pa_hashmap_get(c->namereg, name)) && fail)
        return NULL;

    if (!e)
        n = pa_xstrdup(name);
    else {
        unsigned i;
        size_t l = strlen(name);
        n = pa_xmalloc(l+3);
        
        for (i = 1; i <= 99; i++) {
            snprintf(n, l+2, "%s%u", name, i);

            if (!(e = pa_hashmap_get(c->namereg, n)))
                break;
        }

        if (e) {
            pa_xfree(n);
            return NULL;
        }
    }
    
    assert(n);
    e = pa_xmalloc(sizeof(struct namereg_entry));
    e->type = type;
    e->name = n;
    e->data = data;

    r = pa_hashmap_put(c->namereg, e->name, e);
    assert (r >= 0);

    return e->name;
    
}

void pa_namereg_unregister(pa_core *c, const char *name) {
    struct namereg_entry *e;
    assert(c && name);

    e = pa_hashmap_remove(c->namereg, name);
    assert(e);

    pa_xfree(e->name);
    pa_xfree(e);
}

void* pa_namereg_get(pa_core *c, const char *name, pa_namereg_type_t type, int autoload) {
    struct namereg_entry *e;
    uint32_t idx;
    assert(c);
    
    if (!name) {
        if (type == PA_NAMEREG_SOURCE)
            name = pa_namereg_get_default_source_name(c);
        else if (type == PA_NAMEREG_SINK)
            name = pa_namereg_get_default_sink_name(c);
    }

    if (!name)
        return NULL;
    
    if (c->namereg && (e = pa_hashmap_get(c->namereg, name)))
        if (e->type == e->type)
            return e->data;

    if (pa_atou(name, &idx) < 0) {

        if (autoload) {
            pa_autoload_request(c, name, type);
            
            if (c->namereg && (e = pa_hashmap_get(c->namereg, name)))
                if (e->type == e->type)
                    return e->data;
        }
        
        return NULL;
    }

    if (type == PA_NAMEREG_SINK)
        return pa_idxset_get_by_index(c->sinks, idx);
    else if (type == PA_NAMEREG_SOURCE)
        return pa_idxset_get_by_index(c->sources, idx);
    else if (type == PA_NAMEREG_SAMPLE && c->scache)
        return pa_idxset_get_by_index(c->scache, idx);

    return NULL;
}

void pa_namereg_set_default(pa_core*c, const char *name, pa_namereg_type_t type) {
    char **s;
    assert(c && (type == PA_NAMEREG_SINK || type == PA_NAMEREG_SOURCE));

    s = type == PA_NAMEREG_SINK ? &c->default_sink_name : &c->default_source_name;
    assert(s);

    if (!name && !*s)
        return;
    
    if (name && *s && !strcmp(name, *s))
        return;
    
    pa_xfree(*s);
    *s = pa_xstrdup(name);
    pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SERVER|PA_SUBSCRIPTION_EVENT_CHANGE, PA_INVALID_INDEX);
}

const char *pa_namereg_get_default_sink_name(pa_core *c) {
    pa_sink *s;
    assert(c);

    if (c->default_sink_name)
        return c->default_sink_name;
    
    if ((s = pa_idxset_first(c->sinks, NULL)))
        pa_namereg_set_default(c, s->name, PA_NAMEREG_SINK);

    if (c->default_sink_name)
        return c->default_sink_name;

    return NULL;
}

const char *pa_namereg_get_default_source_name(pa_core *c) {
    pa_source *s;
    uint32_t idx;
    
    assert(c);

    if (c->default_source_name)
        return c->default_source_name;

    for (s = pa_idxset_first(c->sources, &idx); s; s = pa_idxset_next(c->sources, &idx))
        if (!s->monitor_of) {
            pa_namereg_set_default(c, s->name, PA_NAMEREG_SOURCE);
            break;
        }

    if (!c->default_source_name)
        if ((s = pa_idxset_first(c->sources, NULL)))
            pa_namereg_set_default(c, s->name, PA_NAMEREG_SOURCE);

    if (c->default_source_name)
        return c->default_source_name;

    return NULL;
}
