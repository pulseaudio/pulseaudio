/***
  This file is part of PulseAudio.

  Copyright 2011 Colin Guthrie

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

#include <pulse/timeval.h>
#include <pulse/rtclock.h>
#include <pulse/i18n.h>

#include <pulsecore/macro.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/modargs.h>

#include "module-filter-apply-symdef.h"

#define PA_PROP_FILTER_APPLY_MOVING "filter.apply.moving"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("Load filter sinks automatically when needed");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(_("autoclean=<automatically unload unused filters?>"));

static const char* const valid_modargs[] = {
    "autoclean",
    NULL
};

#define DEFAULT_AUTOCLEAN TRUE
#define HOUSEKEEPING_INTERVAL (10 * PA_USEC_PER_SEC)

struct filter {
    char *name;
    pa_sink* parent_sink;
    uint32_t module_index;
    pa_sink* sink;
};

struct userdata {
    pa_core *core;
    pa_hashmap *filters;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_move_finish_slot,
        *sink_input_proplist_slot,
        *sink_input_unlink_slot,
        *sink_unlink_slot;
    pa_bool_t autoclean;
    pa_time_event *housekeeping_time_event;
};

static unsigned filter_hash(const void *p) {
    const struct filter *f = p;

    return
        (unsigned) f->parent_sink->index +
        pa_idxset_string_hash_func(f->name);
}

static int filter_compare(const void *a, const void *b) {
    const struct filter *fa = a, *fb = b;
    int r;

    if (fa->parent_sink != fb->parent_sink)
        return 1;
    if ((r = strcmp(fa->name, fb->name)))
        return r;

    return 0;
}

static struct filter *filter_new(const char *name, pa_sink* parent_sink) {
    struct filter *f;

    f = pa_xnew(struct filter, 1);
    f->name = pa_xstrdup(name);
    pa_assert_se(f->parent_sink = parent_sink);
    f->module_index = PA_INVALID_INDEX;
    f->sink = NULL;
    return f;
}

static void filter_free(struct filter *f) {
    pa_assert(f);

    pa_xfree(f->name);
    pa_xfree(f);
}

static const char* should_filter(pa_sink_input *i) {
    const char *apply;

    /* If the stream doesn't what any filter, then let it be. */
    if ((apply = pa_proplist_gets(i->proplist, PA_PROP_FILTER_APPLY)) && !pa_streq(apply, "")) {
        const char* suppress = pa_proplist_gets(i->proplist, PA_PROP_FILTER_SUPPRESS);

        if (!suppress || !pa_streq(suppress, apply))
            return apply;
    }

    return NULL;
}

static void housekeeping_time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;
    struct filter *filter;
    void *state;

    pa_assert(a);
    pa_assert(e);
    pa_assert(u);

    pa_assert(e == u->housekeeping_time_event);
    u->core->mainloop->time_free(u->housekeeping_time_event);
    u->housekeeping_time_event = NULL;

    PA_HASHMAP_FOREACH(filter, u->filters, state) {
        if (filter->sink && pa_idxset_size(filter->sink->inputs) == 0) {
            uint32_t idx;

            pa_log_debug("Detected filter %s as no longer used on sink %s. Unloading.", filter->name, filter->sink->name);
            idx = filter->module_index;
            pa_hashmap_remove(u->filters, filter);
            filter_free(filter);
            pa_module_unload_request_by_index(u->core, idx, TRUE);
        }
    }

    pa_log_info("Housekeeping Done.");
}

static void trigger_housekeeping(struct userdata *u) {
    pa_assert(u);

    if (!u->autoclean)
        return;

    if (u->housekeeping_time_event)
        return;

    u->housekeeping_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + HOUSEKEEPING_INTERVAL, housekeeping_time_callback, u);
}

static void move_input_for_filter(pa_sink_input *i, struct filter* filter, pa_bool_t restore) {
    pa_sink *sink;

    pa_assert(i);
    pa_assert(filter);

    pa_assert_se(sink = (restore ? filter->parent_sink : filter->sink));

    pa_proplist_sets(i->proplist, PA_PROP_FILTER_APPLY_MOVING, "1");

    if (pa_sink_input_move_to(i, sink, FALSE) < 0)
        pa_log_info("Failed to move sink input %u \"%s\" to <%s>.", i->index,
                    pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), sink->name);
    else
        pa_log_info("Sucessfully moved sink input %u \"%s\" to <%s>.", i->index,
                    pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), sink->name);

    pa_proplist_unset(i->proplist, PA_PROP_FILTER_APPLY_MOVING);
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i) {
    const char *want;
    pa_bool_t done_something = FALSE;

    pa_assert(u);
    pa_sink_input_assert_ref(i);

    /* If there is no sink yet, we can't do much */
    if (!i->sink)
        return PA_HOOK_OK;

    /* If the stream doesn't what any filter, then let it be. */
    if ((want = should_filter(i))) {
        char *module_name;
        struct filter *fltr, *filter;

        /* We need to ensure the SI is playing on a sink of this type
         * attached to the sink it's "officially" playing on */

        if (!i->sink->module)
            return PA_HOOK_OK;

        module_name = pa_sprintf_malloc("module-%s", want);
        if (pa_streq(i->sink->module->name, module_name)) {
            pa_log_debug("Stream appears to be playing on an appropriate sink already. Ignoring.");
            pa_xfree(module_name);
            return PA_HOOK_OK;
        }

        fltr = filter_new(want, i->sink);

        if (!(filter = pa_hashmap_get(u->filters, fltr))) {
            char *args;
            pa_module *m;

            args = pa_sprintf_malloc("sink_master=%s", i->sink->name);
            pa_log_debug("Loading %s with arguments '%s'", module_name, args);

            if ((m = pa_module_load(u->core, module_name, args))) {
                uint32_t idx;
                pa_sink *sink;

                fltr->module_index = m->index;
                /* We cannot use the SINK_PUT hook here to detect our sink as it'll
                 * be called during the module load so we wont yet have put the filter
                 * in our hashmap to compare... so we have to search for it */
                PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
                    if (sink->module == m) {
                        fltr->sink = sink;
                        break;
                    }
                }
                pa_hashmap_put(u->filters, fltr, fltr);
                filter = fltr;
                fltr = NULL;
                done_something = TRUE;
            }
            pa_xfree(args);
        }
        pa_xfree(fltr);

        if (!filter) {
            pa_log("Unable to load %s for sink <%s>", module_name, i->sink->name);
            pa_xfree(module_name);
            return PA_HOOK_OK;
        }
        pa_xfree(module_name);

        if (filter->sink) {
            /* We can move the sink_input now as the know the destination.
             * If this isn't true, we will do it later when the sink appears. */
            move_input_for_filter(i, filter, FALSE);
            done_something = TRUE;
        }
    } else {
        void *state;
        struct filter *filter = NULL;

        /* We do not want to filter... but are we already filtered?
         * This can happen if an input's proplist changes */
        PA_HASHMAP_FOREACH(filter, u->filters, state) {
            if (i->sink == filter->sink) {
                move_input_for_filter(i, filter, TRUE);
                done_something = TRUE;
                break;
            }
        }
    }

    if (done_something)
        trigger_housekeeping(u);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (pa_proplist_gets(i->proplist, PA_PROP_FILTER_APPLY_MOVING))
        return PA_HOOK_OK;

    return process(u, i);
}

static pa_hook_result_t sink_input_proplist_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i);
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    pa_assert(u);

    if (pa_hashmap_size(u->filters) > 0)
        trigger_housekeeping(u);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_unlink_cb(pa_core *core, pa_sink *sink, struct userdata *u) {
    void *state;
    struct filter *filter = NULL;

    pa_core_assert_ref(core);
    pa_sink_assert_ref(sink);
    pa_assert(u);

    /* If either the parent or the sink we've loaded disappears,
     * we should remove it from our hashmap */
    PA_HASHMAP_FOREACH(filter, u->filters, state) {
        if (filter->parent_sink == sink || filter->sink == sink) {
            uint32_t idx;

            /* Attempt to rescue any streams to the parent sink as this is likely
             * the best course of action (as opposed to a generic rescue via
             * module-rescue-streams */
            if (filter->sink == sink) {
                pa_sink_input *i;

                PA_IDXSET_FOREACH(i, sink->inputs, idx)
                    move_input_for_filter(i, filter, TRUE);
            }

            idx = filter->module_index;
            pa_hashmap_remove(u->filters, filter);
            filter_free(filter);
            pa_module_unload_request_by_index(u->core, idx, TRUE);
        }
    }

    return PA_HOOK_OK;
}


int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);

    u->core = m->core;

    u->autoclean = DEFAULT_AUTOCLEAN;
    if (pa_modargs_get_value_boolean(ma, "autoclean", &u->autoclean) < 0) {
        pa_log("Failed to parse autoclean value");
        goto fail;
    }

    u->filters = pa_hashmap_new(filter_hash, filter_compare);

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);
    u->sink_input_proplist_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_proplist_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_unlink_cb, u);

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);
    if (u->sink_input_proplist_slot)
        pa_hook_slot_free(u->sink_input_proplist_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_unlink_slot)
        pa_hook_slot_free(u->sink_unlink_slot);

    if (u->housekeeping_time_event)
        u->core->mainloop->time_free(u->housekeeping_time_event);

    if (u->filters) {
        struct filter *f;

        while ((f = pa_hashmap_steal_first(u->filters))) {
            pa_module_unload_request_by_index(u->core, f->module_index, TRUE);
            filter_free(f);
        }

        pa_hashmap_free(u->filters, NULL, NULL);
    }

    pa_xfree(u);
}
