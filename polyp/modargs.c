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

#include <ctype.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "hashmap.h"
#include "modargs.h"
#include "idxset.h"
#include "sample-util.h"
#include "namereg.h"
#include "sink.h"
#include "source.h"
#include "xmalloc.h"
#include "util.h"

struct pa_modargs;

struct entry {
    char *key, *value;
};

static int add_key_value(struct pa_hashmap *map, char *key, char *value, const char* const valid_keys[]) {
    struct entry *e;
    assert(map && key && value);

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
    
    e = pa_xmalloc(sizeof(struct entry));
    e->key = key;
    e->value = value;
    pa_hashmap_put(map, key, e);
    return 0;
}

struct pa_modargs *pa_modargs_new(const char *args, const char* const* valid_keys) {
    struct pa_hashmap *map = NULL;

    map = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    assert(map);

    if (args) {
        enum { WHITESPACE, KEY, VALUE_START, VALUE_SIMPLE, VALUE_DOUBLE_QUOTES, VALUE_TICKS } state;
        const char *p, *key, *value;
        size_t key_len = 0, value_len = 0;
        
        key = value = NULL;
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
                        if (add_key_value(map, pa_xstrndup(key, key_len), pa_xstrdup(""), valid_keys) < 0)
                            goto fail;
                        state = WHITESPACE;
                    } else {
                        state = VALUE_SIMPLE;
                        value = p;
                        value_len = 1;
                    }
                    break;
                case VALUE_SIMPLE:
                    if (isspace(*p)) {
                        if (add_key_value(map, pa_xstrndup(key, key_len), pa_xstrndup(value, value_len), valid_keys) < 0)
                            goto fail;
                        state = WHITESPACE;
                    } else
                        value_len++;
                    break;
                case VALUE_DOUBLE_QUOTES:
                    if (*p == '"') {
                        if (add_key_value(map, pa_xstrndup(key, key_len), pa_xstrndup(value, value_len), valid_keys) < 0)
                            goto fail;
                        state = WHITESPACE;
                    } else
                        value_len++;
                    break;
                case VALUE_TICKS:
                    if (*p == '\'') {
                        if (add_key_value(map, pa_xstrndup(key, key_len), pa_xstrndup(value, value_len), valid_keys) < 0)
                            goto fail;
                        state = WHITESPACE;
                    } else
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
    }

    return (struct pa_modargs*) map;

fail:

    if (map)
        pa_modargs_free((struct pa_modargs*) map);
                      
    return NULL;
}


static void free_func(void *p, void*userdata) {
    struct entry *e = p;
    assert(e);
    pa_xfree(e->key);
    pa_xfree(e->value);
    pa_xfree(e);
}

void pa_modargs_free(struct pa_modargs*ma) {
    struct pa_hashmap *map = (struct pa_hashmap*) ma;
    pa_hashmap_free(map, free_func, NULL);
}

const char *pa_modargs_get_value(struct pa_modargs *ma, const char *key, const char *def) {
    struct pa_hashmap *map = (struct pa_hashmap*) ma;
    struct entry*e;

    if (!(e = pa_hashmap_get(map, key)))
        return def;

    return e->value;
}

int pa_modargs_get_value_u32(struct pa_modargs *ma, const char *key, uint32_t *value) {
    const char *v;
    char *e;
    unsigned long l;
    assert(ma && key && value);

    if (!(v = pa_modargs_get_value(ma, key, NULL)))
        return 0;

    if (!*v)
        return -1;
    
    l = strtoul(v, &e, 0);
    if (*e)
        return -1;

    *value = (uint32_t) l;
    return 0;
}

int pa_modargs_get_value_s32(struct pa_modargs *ma, const char *key, int32_t *value) {
    const char *v;
    char *e;
    signed long l;
    assert(ma && key && value);

    if (!(v = pa_modargs_get_value(ma, key, NULL)))
        return 0;

    if (!*v)
        return -1;
    
    l = strtol(v, &e, 0);
    if (*e)
        return -1;

    *value = (int32_t) l;
    return 0;
}

int pa_modargs_get_value_boolean(struct pa_modargs *ma, const char *key, int *value) {
    const char *v;
    int r;
    assert(ma && key && value);

    if (!(v = pa_modargs_get_value(ma, key, NULL)))
        return 0;

    if (!*v)
        return -1;

    if ((r = pa_parse_boolean(v)) < 0)
        return -1;

    *value = r;
    return 0;
}

int pa_modargs_get_sample_spec(struct pa_modargs *ma, struct pa_sample_spec *rss) {
    const char *format;
    uint32_t channels;
    struct pa_sample_spec ss;
    assert(ma && rss);

/*    DEBUG_TRAP;*/
    
    ss = *rss;
    if ((pa_modargs_get_value_u32(ma, "rate", &ss.rate)) < 0)
        return -1;

    channels = ss.channels;
    if ((pa_modargs_get_value_u32(ma, "channels", &channels)) < 0)
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
