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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/core-util.h>

#include "module-match-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Playback stream expression matching module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE("table=<filename> "
                "key=<property_key>");

#define WHITESPACE "\n\r \t"

#define DEFAULT_MATCH_TABLE_FILE PA_DEFAULT_CONFIG_DIR"/match.table"
#define DEFAULT_MATCH_TABLE_FILE_USER "match.table"

static const char* const valid_modargs[] = {
    "table",
    "key",
    NULL,
};

struct rule {
    regex_t regex;
    pa_volume_t volume;
    pa_proplist *proplist;
    struct rule *next;
};

struct userdata {
    struct rule *rules;
    char *property_key;
    pa_subscription *subscription;
};

static int load_rules(struct userdata *u, const char *filename) {
    FILE *f;
    int n = 0;
    int ret = -1;
    struct rule *end = NULL;
    char *fn = NULL;

    pa_assert(u);

    if (filename)
        f = fopen(fn = pa_xstrdup(filename), "r");
    else
        f = pa_open_config_file(DEFAULT_MATCH_TABLE_FILE, DEFAULT_MATCH_TABLE_FILE_USER, NULL, &fn);

    if (!f) {
        pa_xfree(fn);
        pa_log("Failed to open file config file: %s", pa_cstrerror(errno));
        goto finish;
    }

    pa_lock_fd(fileno(f), 1);

    while (!feof(f)) {
        char *d, *v;
        pa_volume_t volume = PA_VOLUME_NORM;
        uint32_t k;
        regex_t regex;
        char ln[256];
        struct rule *rule;
        pa_proplist *proplist = NULL;

        if (!fgets(ln, sizeof(ln), f))
            break;

        n++;

        pa_strip_nl(ln);

        if (ln[0] == '#' || !*ln )
            continue;

        d = ln+strcspn(ln, WHITESPACE);
        v = d+strspn(d, WHITESPACE);


        if (!*v) {
            pa_log(__FILE__ ": [%s:%u] failed to parse line - too few words", filename, n);
            goto finish;
        }

        *d = 0;
        if (pa_atou(v, &k) >= 0) {
            volume = (pa_volume_t) k;
        } else if (*v == '"') {
            char *e;

            e = strchr(v+1, '"');
            if (!e) {
                pa_log(__FILE__ ": [%s:%u] failed to parse line - missing role closing quote", filename, n);
                goto finish;
            }

            *e = '\0';
            e = pa_sprintf_malloc("media.role=\"%s\"", v+1);
            proplist = pa_proplist_from_string(e);
            pa_xfree(e);
        } else {
            char *e;

            e = v+strspn(v, WHITESPACE);
            if (!*e) {
                pa_log(__FILE__ ": [%s:%u] failed to parse line - missing end of property list", filename, n);
                goto finish;
            }
            *e = '\0';
            proplist = pa_proplist_from_string(v);
        }

        if (regcomp(&regex, ln, REG_EXTENDED|REG_NOSUB) != 0) {
            pa_log("[%s:%u] invalid regular expression", filename, n);
            goto finish;
        }

        rule = pa_xnew(struct rule, 1);
        rule->regex = regex;
        rule->proplist = proplist;
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
    if (f) {
        pa_lock_fd(fileno(f), 0);
        fclose(f);
    }

    if (fn)
        pa_xfree(fn);

    return ret;
}

static void callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u =  userdata;
    pa_sink_input *si;
    struct rule *r;
    const char *n;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW))
        return;

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, idx)))
        return;

    if (!(n = pa_proplist_gets(si->proplist, u->property_key)))
        return;

    pa_log_debug("Matching with %s", n);

    for (r = u->rules; r; r = r->next) {
        if (!regexec(&r->regex, n, 0, NULL, 0)) {
            if (r->proplist) {
                pa_log_debug("updating proplist of sink input '%s'", n);
                pa_proplist_update(si->proplist, PA_UPDATE_MERGE, r->proplist);
            } else {
                pa_cvolume cv;
                pa_log_debug("changing volume of sink input '%s' to 0x%03x", n, r->volume);
                pa_cvolume_set(&cv, si->sample_spec.channels, r->volume);
                pa_sink_input_set_volume(si, &cv, TRUE, FALSE);
            }
        }
    }
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    u = pa_xnew(struct userdata, 1);
    u->rules = NULL;
    u->subscription = NULL;
    m->userdata = u;

    u->property_key = pa_xstrdup(pa_modargs_get_value(ma, "key", PA_PROP_MEDIA_NAME));

    if (load_rules(u, pa_modargs_get_value(ma, "table", NULL)) < 0)
        goto fail;

    /* FIXME: Doing this asynchronously is just broken. This needs to
     * use a hook! */

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK_INPUT, callback, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);
    return  -1;
}

void pa__done(pa_module*m) {
    struct userdata* u;
    struct rule *r, *n;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);

    if (u->property_key)
        pa_xfree(u->property_key);

    for (r = u->rules; r; r = n) {
        n = r->next;

        regfree(&r->regex);
        if (r->proplist)
            pa_proplist_free(r->proplist);
        pa_xfree(r);
    }

    pa_xfree(u);
}
