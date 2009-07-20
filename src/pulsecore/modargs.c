/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "modargs.h"

struct entry {
    char *key, *value;
};

static int add_key_value(pa_hashmap *map, char *key, char *value, const char* const valid_keys[]) {
    struct entry *e;

    pa_assert(map);
    pa_assert(key);
    pa_assert(value);

    if (pa_hashmap_get(map, key)) {
        pa_xfree(key);
        pa_xfree(value);
        return -1;
    }

    if (valid_keys) {
        const char*const* v;
        for (v = valid_keys; *v; v++)
            if (strcmp(*v, key) == 0)
                break;

        if (!*v) {
            pa_xfree(key);
            pa_xfree(value);
            return -1;
        }
    }

    e = pa_xnew(struct entry, 1);
    e->key = key;
    e->value = value;
    pa_hashmap_put(map, key, e);

    return 0;
}

pa_modargs *pa_modargs_new(const char *args, const char* const* valid_keys) {
    enum {
        WHITESPACE,
        KEY,
        VALUE_START,
        VALUE_SIMPLE,
        VALUE_SIMPLE_ESCAPED,
        VALUE_DOUBLE_QUOTES,
        VALUE_DOUBLE_QUOTES_ESCAPED,
        VALUE_TICKS,
        VALUE_TICKS_ESCAPED
    } state;

    const char *p, *key = NULL, *value = NULL;
    size_t key_len = 0, value_len = 0;
    pa_hashmap *map;

    map = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    if (!args)
        return (pa_modargs*) map;

    state = WHITESPACE;

    for (p = args; *p; p++) {
        switch (state) {

            case WHITESPACE:
                if (*p == '=')
                    goto fail;
                else if (!isspace(*p)) {
                    key = p;
                    state = KEY;
                    key_len = 1;
                }
                break;

            case KEY:
                if (*p == '=')
                    state = VALUE_START;
                else if (isspace(*p))
                    goto fail;
                else
                    key_len++;
                break;

            case  VALUE_START:
                if (*p == '\'') {
                    state = VALUE_TICKS;
                    value = p+1;
                    value_len = 0;
                } else if (*p == '"') {
                    state = VALUE_DOUBLE_QUOTES;
                    value = p+1;
                    value_len = 0;
                } else if (isspace(*p)) {
                    if (add_key_value(map,
                                      pa_xstrndup(key, key_len),
                                      pa_xstrdup(""),
                                      valid_keys) < 0)
                        goto fail;
                    state = WHITESPACE;
                } else if (*p == '\\') {
                    state = VALUE_SIMPLE_ESCAPED;
                    value = p;
                    value_len = 1;
                } else {
                    state = VALUE_SIMPLE;
                    value = p;
                    value_len = 1;
                }
                break;

            case VALUE_SIMPLE:
                if (isspace(*p)) {
                    if (add_key_value(map,
                                      pa_xstrndup(key, key_len),
                                      pa_unescape(pa_xstrndup(value, value_len)),
                                      valid_keys) < 0)
                        goto fail;
                    state = WHITESPACE;
                } else if (*p == '\\') {
                    state = VALUE_SIMPLE_ESCAPED;
                    value_len++;
                } else
                    value_len++;
                break;

            case VALUE_SIMPLE_ESCAPED:
                state = VALUE_SIMPLE;
                value_len++;
                break;

            case VALUE_DOUBLE_QUOTES:
                if (*p == '"') {
                    if (add_key_value(map,
                                      pa_xstrndup(key, key_len),
                                      pa_unescape(pa_xstrndup(value, value_len)),
                                      valid_keys) < 0)
                        goto fail;
                    state = WHITESPACE;
                } else if (*p == '\\') {
                    state = VALUE_DOUBLE_QUOTES_ESCAPED;
                    value_len++;
                } else
                    value_len++;
                break;

            case VALUE_DOUBLE_QUOTES_ESCAPED:
                state = VALUE_DOUBLE_QUOTES;
                value_len++;
                break;

            case VALUE_TICKS:
                if (*p == '\'') {
                    if (add_key_value(map,
                                      pa_xstrndup(key, key_len),
                                      pa_unescape(pa_xstrndup(value, value_len)),
                                      valid_keys) < 0)
                        goto fail;
                    state = WHITESPACE;
                } else if (*p == '\\') {
                    state = VALUE_TICKS_ESCAPED;
                    value_len++;
                } else
                    value_len++;
                break;

            case VALUE_TICKS_ESCAPED:
                state = VALUE_TICKS;
                value_len++;
                break;
        }
    }

    if (state == VALUE_START) {
        if (add_key_value(map, pa_xstrndup(key, key_len), pa_xstrdup(""), valid_keys) < 0)
            goto fail;
    } else if (state == VALUE_SIMPLE) {
        if (add_key_value(map, pa_xstrndup(key, key_len), pa_xstrdup(value), valid_keys) < 0)
            goto fail;
    } else if (state != WHITESPACE)
        goto fail;

    return (pa_modargs*) map;

fail:

    pa_modargs_free((pa_modargs*) map);

    return NULL;
}

static void free_func(void *p, void*userdata) {
    struct entry *e = p;
    pa_assert(e);

    pa_xfree(e->key);
    pa_xfree(e->value);
    pa_xfree(e);
}

void pa_modargs_free(pa_modargs*ma) {
    pa_hashmap *map = (pa_hashmap*) ma;
    pa_hashmap_free(map, free_func, NULL);
}

const char *pa_modargs_get_value(pa_modargs *ma, const char *key, const char *def) {
    pa_hashmap *map = (pa_hashmap*) ma;
    struct entry*e;

    if (!(e = pa_hashmap_get(map, key)))
        return def;

    return e->value;
}

int pa_modargs_get_value_u32(pa_modargs *ma, const char *key, uint32_t *value) {
    const char *v;

    pa_assert(ma);
    pa_assert(key);
    pa_assert(value);

    if (!(v = pa_modargs_get_value(ma, key, NULL)))
        return 0;

    if (pa_atou(v, value) < 0)
        return -1;

    return 0;
}

int pa_modargs_get_value_s32(pa_modargs *ma, const char *key, int32_t *value) {
    const char *v;

    pa_assert(ma);
    pa_assert(key);
    pa_assert(value);

    if (!(v = pa_modargs_get_value(ma, key, NULL)))
        return 0;

    if (pa_atoi(v, value) < 0)
        return -1;

    return 0;
}

int pa_modargs_get_value_boolean(pa_modargs *ma, const char *key, pa_bool_t *value) {
    const char *v;
    int r;

    pa_assert(ma);
    pa_assert(key);
    pa_assert(value);

    if (!(v = pa_modargs_get_value(ma, key, NULL)))
        return 0;

    if (!*v)
        return -1;

    if ((r = pa_parse_boolean(v)) < 0)
        return -1;

    *value = r;
    return 0;
}

int pa_modargs_get_sample_spec(pa_modargs *ma, pa_sample_spec *rss) {
    const char *format;
    uint32_t channels;
    pa_sample_spec ss;

    pa_assert(ma);
    pa_assert(rss);

    ss = *rss;
    if ((pa_modargs_get_value_u32(ma, "rate", &ss.rate)) < 0 ||
        ss.rate <= 0 ||
        ss.rate > PA_RATE_MAX)
        return -1;

    channels = ss.channels;
    if ((pa_modargs_get_value_u32(ma, "channels", &channels)) < 0 ||
        channels <= 0 ||
        channels >= PA_CHANNELS_MAX)
        return -1;
    ss.channels = (uint8_t) channels;

    if ((format = pa_modargs_get_value(ma, "format", NULL)))
        if ((ss.format = pa_parse_sample_format(format)) < 0)
            return -1;

    if (!pa_sample_spec_valid(&ss))
        return -1;

    *rss = ss;

    return 0;
}

int pa_modargs_get_channel_map(pa_modargs *ma, const char *name, pa_channel_map *rmap) {
    pa_channel_map map;
    const char *cm;

    pa_assert(ma);
    pa_assert(rmap);

    map = *rmap;

    if ((cm = pa_modargs_get_value(ma, name ? name : "channel_map", NULL)))
        if (!pa_channel_map_parse(&map, cm))
            return -1;

    if (!pa_channel_map_valid(&map))
        return -1;

    *rmap = map;
    return 0;
}

int pa_modargs_get_sample_spec_and_channel_map(
        pa_modargs *ma,
        pa_sample_spec *rss,
        pa_channel_map *rmap,
        pa_channel_map_def_t def) {

    pa_sample_spec ss;
    pa_channel_map map;

    pa_assert(ma);
    pa_assert(rss);
    pa_assert(rmap);

    ss = *rss;

    if (pa_modargs_get_sample_spec(ma, &ss) < 0)
        return -1;

    map = *rmap;

    if (ss.channels != map.channels)
        pa_channel_map_init_extend(&map, ss.channels, def);

    if (pa_modargs_get_channel_map(ma, NULL, &map) < 0)
        return -1;

    if (map.channels != ss.channels)
        return -1;

    *rmap = map;
    *rss = ss;

    return 0;
}

int pa_modargs_get_proplist(pa_modargs *ma, const char *name, pa_proplist *p, pa_update_mode_t m) {
    const char *v;
    pa_proplist *n;

    pa_assert(ma);
    pa_assert(name);
    pa_assert(p);

    if (!(v = pa_modargs_get_value(ma, name, NULL)))
        return 0;

    if (!(n = pa_proplist_from_string(v)))
        return -1;

    pa_proplist_update(p, m, n);
    pa_proplist_free(n);

    return 0;
}
