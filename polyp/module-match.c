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

#include "module.h"
#include "util.h"
#include "modargs.h"
#include "log.h"
#include "subscribe.h"
#include "xmalloc.h"
#include "sink-input.h"
#include "module-match-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Sink input matching module")
PA_MODULE_USAGE("table=<filename>")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define WHITESPACE "\n\r \t"

#ifndef DEFAULT_CONFIG_DIR
#define DEFAULT_CONFIG_DIR "/etc/polypaudio"
#endif

#define DEFAULT_MATCH_TABLE_FILE DEFAULT_CONFIG_DIR"/match.table"
#define DEFAULT_MATCH_TABLE_FILE_USER ".polypaudio/match.table"

static const char* const valid_modargs[] = {
    "table",
    NULL,
};

struct rule {
    regex_t regex;
    pa_volume_t volume;
    struct rule *next;
};

struct userdata {
    struct rule *rules;
    struct pa_subscription *subscription;
};

static int load_rules(struct userdata *u, const char *filename) {
    FILE *f;
    int n = 0;
    int ret = -1;
    struct rule *end = NULL;
    char *fn = NULL;

    f = filename ?
        fopen(fn = pa_xstrdup(filename), "r") :
        pa_open_config_file(DEFAULT_MATCH_TABLE_FILE, DEFAULT_MATCH_TABLE_FILE_USER, NULL, &fn);

    if (!f) {
        pa_log(__FILE__": failed to open file '%s': %s\n", fn, strerror(errno));
        goto finish;
    }

    while (!feof(f)) {
        char *d, *v, *e = NULL;
        pa_volume_t volume;
        regex_t regex;
        char ln[256];
        struct rule *rule;
        
        if (!fgets(ln, sizeof(ln), f))
            break;

        n++;
        
        pa_strip_nl(ln);

        if (ln[0] == '#' || !*ln )
            continue;

        d = ln+strcspn(ln, WHITESPACE);
        v = d+strspn(d, WHITESPACE);

        
        if (!*v) {
            pa_log(__FILE__ ": [%s:%u] failed to parse line - too few words\n", filename, n);
            goto finish;
        }

        *d = 0;
        
        volume = (pa_volume_t) strtol(v, &e, 0);

        if (!e || *e) {
            pa_log(__FILE__": [%s:%u] failed to parse volume\n", filename, n);
            goto finish;
        }

        if (regcomp(&regex, ln, REG_EXTENDED|REG_NOSUB) != 0) {
            pa_log(__FILE__": [%s:%u] invalid regular expression\n", filename, n);
            goto finish;
        }

        rule = pa_xmalloc(sizeof(struct rule));
        rule->regex = regex;
        rule->volume = volume;
        rule->next = NULL;

        if (end)
            end->next = rule;
        else
            u->rules = rule;
        end = rule;
        
        *d = 0;
    }

    ret = 0;
    
finish:
    if (f)
        fclose(f);

    if (fn)
        pa_xfree(fn);

    return ret;
}

static void callback(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata) {
    struct userdata *u =  userdata;
    struct pa_sink_input *si;
    struct rule *r;
    assert(c && u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW))
        return;

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, index))) {
        pa_log(__FILE__": WARNING: failed to get sink input\n");
        return;
    }

    if (!si->name)
        return;
    
    for (r = u->rules; r; r = r->next) {
        if (!regexec(&r->regex, si->name, 0, NULL, 0)) {
            pa_log(__FILE__": changing volume of sink input '%s' to 0x%03x\n", si->name, r->volume);
            pa_sink_input_set_volume(si, r->volume);
        }
    }
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    const char *table_file;
    struct userdata *u;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs)) ||
        !(table_file = pa_modargs_get_value(ma, "table", NULL))) {
        pa_log(__FILE__": Failed to parse module arguments\n");
        goto fail;
    }

    u = pa_xmalloc(sizeof(struct userdata));
    u->rules = NULL;
    u->subscription = NULL;
    m->userdata = u;
    
    if (load_rules(u, table_file) < 0)
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

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata* u;
    struct rule *r, *n;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);
    
    for (r = u->rules; r; r = n) {
        n = r->next;

        regfree(&r->regex);
        pa_xfree(r);
    }

    pa_xfree(u);
}


