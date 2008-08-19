/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <pulse/xmalloc.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>

#include "module-volume-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the volume and the devices of streams");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "table=<filename> "
        "restore_device=<Restore the device for each stream?> "
        "restore_volume=<Restore the volume for each stream?>"
);

#define WHITESPACE "\n\r \t"
#define DEFAULT_VOLUME_TABLE_FILE "volume-restore.table"
#define SAVE_INTERVAL 10

static const char* const valid_modargs[] = {
    "table",
    "restore_device",
    "restore_volume",
    NULL,
};

struct rule {
    char* name;
    pa_bool_t volume_is_set;
    pa_cvolume volume;
    char *sink, *source;
};

struct userdata {
    pa_core *core;
    pa_hashmap *hashmap;
    pa_subscription *subscription;
    pa_hook_slot
        *sink_input_new_hook_slot,
        *sink_input_fixate_hook_slot,
        *source_output_new_hook_slot;
    pa_bool_t modified;
    char *table_file;
    pa_time_event *save_time_event;
};

static pa_cvolume* parse_volume(const char *s, pa_cvolume *v) {
    char *p;
    long k;
    unsigned i;

    pa_assert(s);
    pa_assert(v);

    if (!isdigit(*s))
        return NULL;

    k = strtol(s, &p, 0);
    if (k <= 0 || k > (long) PA_CHANNELS_MAX)
        return NULL;

    v->channels = (uint8_t) k;

    for (i = 0; i < v->channels; i++) {
        p += strspn(p, WHITESPACE);

        if (!isdigit(*p))
            return NULL;

        k = strtol(p, &p, 0);

        if (k < (long) PA_VOLUME_MUTED)
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
    char buf_name[256], buf_volume[256], buf_sink[256], buf_source[256];
    char *ln = buf_name;

    if (!(f = fopen(u->table_file, "r"))) {
        if (errno == ENOENT) {
            pa_log_info("Starting with empty ruleset.");
            ret = 0;
        } else
            pa_log("Failed to open file '%s': %s", u->table_file, pa_cstrerror(errno));

        goto finish;
    }

    pa_lock_fd(fileno(f), 1);

    while (!feof(f)) {
        struct rule *rule;
        pa_cvolume v;
        pa_bool_t v_is_set;

        if (!fgets(ln, sizeof(buf_name), f))
            break;

        n++;

        pa_strip_nl(ln);

        if (ln[0] == '#')
            continue;

        if (ln == buf_name) {
            ln = buf_volume;
            continue;
        }

        if (ln == buf_volume) {
            ln = buf_sink;
            continue;
        }

        if (ln == buf_sink) {
            ln = buf_source;
            continue;
        }

        pa_assert(ln == buf_source);

        if (buf_volume[0]) {
            if (!parse_volume(buf_volume, &v)) {
                pa_log("parse failure in %s:%u, stopping parsing", u->table_file, n);
                goto finish;
            }

            v_is_set = TRUE;
        } else
            v_is_set = FALSE;

        ln = buf_name;

        if (pa_hashmap_get(u->hashmap, buf_name)) {
            pa_log("double entry in %s:%u, ignoring", u->table_file, n);
            continue;
        }

        rule = pa_xnew(struct rule, 1);
        rule->name = pa_xstrdup(buf_name);
        if ((rule->volume_is_set = v_is_set))
            rule->volume = v;
        rule->sink = buf_sink[0] ? pa_xstrdup(buf_sink) : NULL;
        rule->source = buf_source[0] ? pa_xstrdup(buf_source) : NULL;

        pa_hashmap_put(u->hashmap, rule->name, rule);
    }

    if (ln != buf_name) {
        pa_log("invalid number of lines in %s.", u->table_file);
        goto finish;
    }

    ret = 0;

finish:
    if (f) {
        pa_lock_fd(fileno(f), 0);
        fclose(f);
    }

    return ret;
}

static int save_rules(struct userdata *u) {
    FILE *f;
    int ret = -1;
    void *state = NULL;
    struct rule *rule;

    if (!u->modified)
        return 0;

    pa_log_info("Saving rules...");

    if (!(f = fopen(u->table_file, "w"))) {
        pa_log("Failed to open file '%s': %s", u->table_file, pa_cstrerror(errno));
        goto finish;
    }

    pa_lock_fd(fileno(f), 1);

    while ((rule = pa_hashmap_iterate(u->hashmap, &state, NULL))) {
        unsigned i;

        fprintf(f, "%s\n", rule->name);

        if (rule->volume_is_set) {
            fprintf(f, "%u", rule->volume.channels);

            for (i = 0; i < rule->volume.channels; i++)
                fprintf(f, " %u", rule->volume.values[i]);
        }

        fprintf(f, "\n%s\n%s\n",
                rule->sink ? rule->sink : "",
                rule->source ? rule->source : "");
    }

    ret = 0;
    u->modified = FALSE;
    pa_log_debug("Successfully saved rules...");

finish:
    if (f) {
        pa_lock_fd(fileno(f), 0);
        fclose(f);
    }

    return ret;
}

static char* client_name(pa_client *c) {
    char *t, *e;

    if (!pa_proplist_gets(c->proplist, PA_PROP_APPLICATION_NAME) || !c->driver)
        return NULL;

    t = pa_sprintf_malloc("%s$%s", c->driver, pa_proplist_gets(c->proplist, PA_PROP_APPLICATION_NAME));
    t[strcspn(t, "\n\r#")] = 0;

    if (!*t) {
        pa_xfree(t);
        return NULL;
    }

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

static void save_time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(tv);
    pa_assert(u);

    pa_assert(e == u->save_time_event);
    u->core->mainloop->time_free(u->save_time_event);
    u->save_time_event = NULL;

    save_rules(u);
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u =  userdata;
    pa_sink_input *si = NULL;
    pa_source_output *so = NULL;
    struct rule *r;
    char *name;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        if (!(si = pa_idxset_get_by_index(c->sink_inputs, idx)))
            return;

        if (!si->client || !(name = client_name(si->client)))
            return;
    } else {
        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT);

        if (!(so = pa_idxset_get_by_index(c->source_outputs, idx)))
            return;

        if (!so->client || !(name = client_name(so->client)))
            return;
    }

    if ((r = pa_hashmap_get(u->hashmap, name))) {
        pa_xfree(name);

        if (si) {

            if (!r->volume_is_set || !pa_cvolume_equal(pa_sink_input_get_volume(si), &r->volume)) {
                pa_log_info("Saving volume for <%s>", r->name);
                r->volume = *pa_sink_input_get_volume(si);
                r->volume_is_set = TRUE;
                u->modified = TRUE;
            }

            if (!r->sink || strcmp(si->sink->name, r->sink) != 0) {
                pa_log_info("Saving sink for <%s>", r->name);
                pa_xfree(r->sink);
                r->sink = pa_xstrdup(si->sink->name);
                u->modified = TRUE;
            }
        } else {
            pa_assert(so);

            if (!r->source || strcmp(so->source->name, r->source) != 0) {
                pa_log_info("Saving source for <%s>", r->name);
                pa_xfree(r->source);
                r->source = pa_xstrdup(so->source->name);
                u->modified = TRUE;
            }
        }

    } else {
        pa_log_info("Creating new entry for <%s>", name);

        r = pa_xnew(struct rule, 1);
        r->name = name;

        if (si) {
            r->volume = *pa_sink_input_get_volume(si);
            r->volume_is_set = TRUE;
            r->sink = pa_xstrdup(si->sink->name);
            r->source = NULL;
        } else {
            pa_assert(so);
            r->volume_is_set = FALSE;
            r->sink = NULL;
            r->source = pa_xstrdup(so->source->name);
        }

        pa_hashmap_put(u->hashmap, r->name, r);
        u->modified = TRUE;
    }

    if (u->modified && !u->save_time_event) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec += SAVE_INTERVAL;
        u->save_time_event = u->core->mainloop->time_new(u->core->mainloop, &tv, save_time_callback, u);
    }
}

static pa_hook_result_t sink_input_new_hook_callback(pa_core *c, pa_sink_input_new_data *data, struct userdata *u) {
    struct rule *r;
    char *name;

    pa_assert(data);

    /* In the NEW hook we only adjust the device. Adjusting the volume
     * is left for the FIXATE hook */

    if (!data->client || !(name = client_name(data->client)))
        return PA_HOOK_OK;

    if ((r = pa_hashmap_get(u->hashmap, name))) {
        if (!data->sink && r->sink) {
            if ((data->sink = pa_namereg_get(c, r->sink, PA_NAMEREG_SINK, 1)))
                pa_log_info("Restoring sink for <%s>", r->name);
        }
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_fixate_hook_callback(pa_core *c, pa_sink_input_new_data *data, struct userdata *u) {
    struct rule *r;
    char *name;

    pa_assert(data);

    /* In the FIXATE hook we only adjust the volum. Adjusting the device
     * is left for the NEW hook */

    if (!data->client || !(name = client_name(data->client)))
        return PA_HOOK_OK;

    if ((r = pa_hashmap_get(u->hashmap, name))) {

        if (r->volume_is_set && data->sample_spec.channels == r->volume.channels) {
            pa_log_info("Restoring volume for <%s>", r->name);
            pa_sink_input_new_data_set_volume(data, &r->volume);
        }
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_new_hook_callback(pa_core *c, pa_source_output_new_data *data, struct userdata *u) {
    struct rule *r;
    char *name;

    pa_assert(data);

    if (!data->client || !(name = client_name(data->client)))
        return PA_HOOK_OK;

    if ((r = pa_hashmap_get(u->hashmap, name))) {
        if (!data->source && r->source) {
            if ((data->source = pa_namereg_get(c, r->source, PA_NAMEREG_SOURCE, 1)))
                pa_log_info("Restoring source for <%s>", r->name);
        }
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    pa_bool_t restore_device = TRUE, restore_volume = TRUE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->hashmap = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->modified = FALSE;
    u->subscription = NULL;
    u->sink_input_new_hook_slot = u->sink_input_fixate_hook_slot = u->source_output_new_hook_slot = NULL;
    u->save_time_event = NULL;

    m->userdata = u;

    if (!(u->table_file = pa_state_path(pa_modargs_get_value(ma, "table", DEFAULT_VOLUME_TABLE_FILE), TRUE)))
        goto fail;

    if (pa_modargs_get_value_boolean(ma, "restore_device", &restore_device) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_volume", &restore_volume) < 0) {
        pa_log("restore_volume= and restore_device= expect boolean arguments");
        goto fail;
    }

    if (!(restore_device || restore_volume)) {
        pa_log("Both restrong the volume and restoring the device are disabled. There's no point in using this module at all then, failing.");
        goto fail;
    }

    if (load_rules(u) < 0)
        goto fail;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK_INPUT|PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, subscribe_callback, u);

    if (restore_device) {
        u->sink_input_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_new_hook_callback, u);
        u->source_output_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_output_new_hook_callback, u);
    }

    if (restore_volume)
        u->sink_input_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_fixate_hook_callback, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);
    if (ma)
        pa_modargs_free(ma);

    return  -1;
}

static void free_func(void *p, void *userdata) {
    struct rule *r = p;
    pa_assert(r);

    pa_xfree(r->name);
    pa_xfree(r->sink);
    pa_xfree(r->source);
    pa_xfree(r);
}

void pa__done(pa_module*m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);

    if (u->sink_input_new_hook_slot)
        pa_hook_slot_free(u->sink_input_new_hook_slot);
    if (u->sink_input_fixate_hook_slot)
        pa_hook_slot_free(u->sink_input_fixate_hook_slot);
    if (u->source_output_new_hook_slot)
        pa_hook_slot_free(u->source_output_new_hook_slot);

    if (u->hashmap) {
        save_rules(u);
        pa_hashmap_free(u->hashmap, free_func, NULL);
    }

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    pa_xfree(u->table_file);
    pa_xfree(u);
}
