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
#include <string.h>
#include <stdio.h>

#include <pulse/xmalloc.h>

#include <pulsecore/autoload.h>
#include <pulsecore/source.h>
#include <pulsecore/sink.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "namereg.h"

struct namereg_entry {
    pa_namereg_type_t type;
    char *name;
    void *data;
};

static pa_bool_t is_valid_char(char c) {
    return
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '.' ||
        c == '-' ||
        c == '_';
}

pa_bool_t pa_namereg_is_valid_name(const char *name) {
    const char *c;

    if (*name == 0)
        return FALSE;

    for (c = name; *c && (c-name < PA_NAME_MAX); c++)
        if (!is_valid_char(*c))
            return FALSE;

    if (*c)
        return FALSE;

    return TRUE;
}

char* pa_namereg_make_valid_name(const char *name) {
    const char *a;
    char *b, *n;

    if (*name == 0)
        return NULL;

    n = pa_xmalloc(strlen(name)+1);

    for (a = name, b = n; *a && (a-name < PA_NAME_MAX); a++, b++)
        *b = (char) (is_valid_char(*a) ? *a : '_');

    *b = 0;

    return n;
}

void pa_namereg_free(pa_core *c) {
    pa_assert(c);

    if (!c->namereg)
        return;

    pa_assert(pa_hashmap_size(c->namereg) == 0);
    pa_hashmap_free(c->namereg, NULL, NULL);
}

const char *pa_namereg_register(pa_core *c, const char *name, pa_namereg_type_t type, void *data, pa_bool_t fail) {
    struct namereg_entry *e;
    char *n = NULL;

    pa_assert(c);
    pa_assert(name);
    pa_assert(data);

    if (!*name)
        return NULL;

    if ((type == PA_NAMEREG_SINK || type == PA_NAMEREG_SOURCE) &&
        !pa_namereg_is_valid_name(name)) {

        if (fail)
            return NULL;

        if (!(name = n = pa_namereg_make_valid_name(name)))
            return NULL;
    }

    if (!c->namereg)
        c->namereg = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    if ((e = pa_hashmap_get(c->namereg, name)) && fail) {
        pa_xfree(n);
        return NULL;
    }

    if (e) {
        unsigned i;
        size_t l = strlen(name);
        char *k;

        if (l+4 > PA_NAME_MAX) {
            pa_xfree(n);
            return NULL;
        }

        k = pa_xmalloc(l+4);

        for (i = 2; i <= 99; i++) {
            pa_snprintf(k, l+4, "%s.%u", name, i);

            if (!(e = pa_hashmap_get(c->namereg, k)))
                break;
        }

        if (e) {
            pa_xfree(n);
            pa_xfree(k);
            return NULL;
        }

        pa_xfree(n);
        n = k;
    }

    e = pa_xnew(struct namereg_entry, 1);
    e->type = type;
    e->name = n ? n : pa_xstrdup(name);
    e->data = data;

    pa_assert_se(pa_hashmap_put(c->namereg, e->name, e) >= 0);

    return e->name;
}

void pa_namereg_unregister(pa_core *c, const char *name) {
    struct namereg_entry *e;

    pa_assert(c);
    pa_assert(name);

    pa_assert_se(e = pa_hashmap_remove(c->namereg, name));

    pa_xfree(e->name);
    pa_xfree(e);
}

void* pa_namereg_get(pa_core *c, const char *name, pa_namereg_type_t type, pa_bool_t autoload) {
    struct namereg_entry *e;
    uint32_t idx;
    pa_assert(c);

    if (!name) {

        if (type == PA_NAMEREG_SOURCE)
            name = pa_namereg_get_default_source_name(c);
        else if (type == PA_NAMEREG_SINK)
            name = pa_namereg_get_default_sink_name(c);

    } else if (strcmp(name, "@DEFAULT_SINK@") == 0) {
        if (type == PA_NAMEREG_SINK)
               name = pa_namereg_get_default_sink_name(c);

    } else if (strcmp(name, "@DEFAULT_SOURCE@") == 0) {
        if (type == PA_NAMEREG_SOURCE)
            name = pa_namereg_get_default_source_name(c);

    } else if (strcmp(name, "@DEFAULT_MONITOR@") == 0) {
        if (type == PA_NAMEREG_SOURCE) {
            pa_sink *k;

            if ((k = pa_namereg_get(c, NULL, PA_NAMEREG_SINK, autoload)))
                return k->monitor_source;
        }
    } else if (*name == '@')
        name = NULL;

    if (!name)
        return NULL;

    if (c->namereg && (e = pa_hashmap_get(c->namereg, name)))
        if (e->type == type)
            return e->data;

    if (pa_atou(name, &idx) < 0) {

        if (autoload) {
            pa_autoload_request(c, name, type);

            if (c->namereg && (e = pa_hashmap_get(c->namereg, name)))
                if (e->type == type)
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

int pa_namereg_set_default(pa_core*c, const char *name, pa_namereg_type_t type) {
    char **s;

    pa_assert(c);
    pa_assert(type == PA_NAMEREG_SINK || type == PA_NAMEREG_SOURCE);

    s = type == PA_NAMEREG_SINK ? &c->default_sink_name : &c->default_source_name;

    if (!name && !*s)
        return 0;

    if (name && *s && !strcmp(name, *s))
        return 0;

    if (!pa_namereg_is_valid_name(name))
        return -1;

    pa_xfree(*s);
    *s = pa_xstrdup(name);
    pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SERVER|PA_SUBSCRIPTION_EVENT_CHANGE, PA_INVALID_INDEX);

    return 0;
}

const char *pa_namereg_get_default_sink_name(pa_core *c) {
    pa_sink *s;

    pa_assert(c);

    if (c->default_sink_name)
        return c->default_sink_name;

    if ((s = pa_idxset_first(c->sinks, NULL)))
        pa_namereg_set_default(c, s->name, PA_NAMEREG_SINK);

    return c->default_sink_name;
}

const char *pa_namereg_get_default_source_name(pa_core *c) {
    pa_source *s;
    uint32_t idx;

    pa_assert(c);

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

    return c->default_source_name;
}
