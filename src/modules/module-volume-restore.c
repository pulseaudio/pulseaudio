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

#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <polypcore/module.h>
#include <polypcore/util.h>
#include <polypcore/modargs.h>
#include <polypcore/log.h>
#include <polypcore/core-subscribe.h>
#include <polypcore/xmalloc.h>
#include <polypcore/sink-input.h>
#include <polyp/volume.h>

#include "module-volume-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Playback stream automatic volume restore module")
PA_MODULE_USAGE("table=<filename>")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define WHITESPACE "\n\r \t"

#define DEFAULT_VOLUME_TABLE_FILE ".polypaudio/volume.table"

static const char* const valid_modargs[] = {
    "table",
    NULL,
};

struct rule {
    char* name;
    pa_cvolume volume;
};

struct userdata {
    pa_hashmap *hashmap;
    pa_subscription *subscription;
    int modified;
    char *table_file;
};

static pa_cvolume* parse_volume(const char *s, pa_cvolume *v) {
    char *p;
    long k;
    unsigned i;
    
    assert(s);
    assert(v);

    if (!isdigit(*s))
        return NULL;

    k = strtol(s, &p, 0);
    if (k <= 0 || k > PA_CHANNELS_MAX)
        return NULL;
    
    v->channels = (unsigned) k;

    for (i = 0; i < v->channels; i++) {
        p += strspn(p, WHITESPACE);

        if (!isdigit(*p))
            return NULL;

        k = strtol(p, &p, 0);

        if (k < PA_VOLUME_MUTED)
            return NULL;
        
        v->values[i] = (pa_volume_t) k;
    }

    if (*p != 0)
        return NULL;

    return v;
}

static int load_rules(struct userdata *u) {
    FILE *f;
    int n = 0;
    int ret = -1;
    char buf_name[256], buf_volume[256];
    char *ln = buf_name;

    f = u->table_file ?
        fopen(u->table_file, "r") :
        pa_open_config_file(NULL, DEFAULT_VOLUME_TABLE_FILE, NULL, &u->table_file, "r");
    
    if (!f) {
        if (errno == ENOENT) {
            pa_log(__FILE__": starting with empty ruleset.");
            ret = 0;
        } else
            pa_log(__FILE__": failed to open file '%s': %s", u->table_file, strerror(errno));
        
        goto finish;
    }

    while (!feof(f)) {
        struct rule *rule;
        pa_cvolume v;
        
        if (!fgets(ln, sizeof(buf_name), f))
            break;

        n++;
        
        pa_strip_nl(ln);

        if (ln[0] == '#' || !*ln )
            continue;

        if (ln == buf_name) {
            ln = buf_volume;
            continue;
        }

        assert(ln == buf_volume);

        if (!parse_volume(buf_volume, &v)) {
            pa_log(__FILE__": parse failure in %s:%u, stopping parsing", u->table_file, n);
            goto finish;
        }

        ln = buf_name;
        
        if (pa_hashmap_get(u->hashmap, buf_name)) {
            pa_log(__FILE__": double entry in %s:%u, ignoring", u->table_file, n);
            goto finish;
        }
        
        rule = pa_xnew(struct rule, 1);
        rule->name = pa_xstrdup(buf_name);
        rule->volume = v;

        pa_hashmap_put(u->hashmap, rule->name, rule);
    }

    if (ln == buf_volume) {
        pa_log(__FILE__": invalid number of lines in %s.", u->table_file);
        goto finish;
    }

    ret = 0;
    
finish:
    if (f)
        fclose(f);

    return ret;
}

static int save_rules(struct userdata *u) {
    FILE *f;
    int ret = -1;
    void *state = NULL;
    struct rule *rule;
    
    f = u->table_file ?
        fopen(u->table_file, "w") :
        pa_open_config_file(NULL, DEFAULT_VOLUME_TABLE_FILE, NULL, &u->table_file, "w");

    if (!f) {
        pa_log(__FILE__": failed to open file '%s': %s", u->table_file, strerror(errno));
        goto finish;
    }

    while ((rule = pa_hashmap_iterate(u->hashmap, &state, NULL))) {
        unsigned i;
        
        fprintf(f, "%s\n%u", rule->name, rule->volume.channels);
        
        for (i = 0; i < rule->volume.channels; i++)
            fprintf(f, " %u", rule->volume.values[i]);


        fprintf(f, "\n");
    }
    
    ret = 0;
    
finish:
    if (f)
        fclose(f);

    return ret;
}

static char* client_name(pa_client *c) {
    char *t, *e;
    
    if (!c->name || !c->driver)
        return NULL;

    t = pa_sprintf_malloc("%s$%s", c->driver, c->name);
    t[strcspn(t, "\n\r#")] = 0;

    if (!*t)
        return NULL;

    if ((e = strrchr(t, '('))) {
        char *k = e + 1 + strspn(e + 1, "0123456789-");

        /* Dirty trick: truncate all trailing parens with numbers in
         * between, since they are usually used to identify multiple
         * sessions of the same application, which is something we
         * explicitly don't want. Besides other stuff this makes xmms
         * with esound work properly for us. */
        
        if (*k == ')' && *(k+1) == 0)
            *e = 0;
    }
    
    return t;
}

static void callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u =  userdata;
    pa_sink_input *si;
    struct rule *r;
    char *name;
    
    assert(c);
    assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;
        
    if (!(si = pa_idxset_get_by_index(c->sink_inputs, idx)))
        return;
    
    if (!si->client || !(name = client_name(si->client)))
        return;

    if ((r = pa_hashmap_get(u->hashmap, name))) {
        pa_xfree(name);

        if (((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) && si->sample_spec.channels == r->volume.channels) {
            pa_log_info(__FILE__": Restoring volume for <%s>", r->name);
            pa_sink_input_set_volume(si, &r->volume);
        } else if (!pa_cvolume_equal(pa_sink_input_get_volume(si), &r->volume)) {
            pa_log_info(__FILE__": Saving volume for <%s>", r->name);
            r->volume = *pa_sink_input_get_volume(si);
            u->modified = 1;
        }
        
    } else {
        pa_log_info(__FILE__": Creating new entry for <%s>", name);

        r = pa_xnew(struct rule, 1);
        r->name = name;
        r->volume = *pa_sink_input_get_volume(si);
        pa_hashmap_put(u->hashmap, r->name, r);

        u->modified = 1;
    }
}

int pa__init(pa_core *c, pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    
    assert(c);
    assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": Failed to parse module arguments");
        goto fail;
    }

    u = pa_xnew(struct userdata, 1);
    u->hashmap = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->subscription = NULL;
    u->table_file = pa_xstrdup(pa_modargs_get_value(ma, "table", NULL));
    u->modified = 0;
    
    m->userdata = u;
    
    if (load_rules(u) < 0)
        goto fail;

    u->subscription = pa_subscription_new(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, callback, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(c, m);

    if (ma)
        pa_modargs_free(ma);
    
    return  -1;
}

static void free_func(void *p, void *userdata) {
    struct rule *r = p;
    assert(r);

    pa_xfree(r->name);
    pa_xfree(r);
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata* u;
    
    assert(c);
    assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);

    if (u->hashmap) {

        if (u->modified)
            save_rules(u);
        
        pa_hashmap_free(u->hashmap, free_func, NULL);
    }

    pa_xfree(u->table_file);
    pa_xfree(u);
}


