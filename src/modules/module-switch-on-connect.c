/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2009 Canonical Ltd

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

PA_MODULE_AUTHOR("Michael Terry");
PA_MODULE_DESCRIPTION("When a sink/source is added, switch to it or conditionally switch to it");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "only_from_unavailable=<boolean, only switch from unavailable ports> "
        "ignore_virtual=<boolean, ignore new virtual sinks and sources, defaults to true> "
);

static const char* const valid_modargs[] = {
    "only_from_unavailable",
    "ignore_virtual",
    NULL,
};

struct userdata {
    bool only_from_unavailable;
    bool ignore_virtual;
};

static pa_hook_result_t sink_put_hook_callback(pa_core *c, pa_sink *sink, void* userdata) {
    pa_sink_input *i;
    uint32_t idx;
    pa_sink *old_default_sink;
    const char *s;
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(userdata);

    /* Don't want to run during startup or shutdown */
    if (c->state != PA_CORE_RUNNING)
        return PA_HOOK_OK;

    pa_log_debug("Trying to switch to new sink %s", sink->name);

    /* Don't switch to any internal devices except HDMI */
    s = pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_STRING);
    if (s && !pa_startswith(s, "hdmi")) {
        s = pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_BUS);
        if (pa_safe_streq(s, "pci") || pa_safe_streq(s, "isa")) {
            pa_log_debug("Refusing to switch to sink on %s bus", s);
            return PA_HOOK_OK;
        }
    }

    /* Ignore virtual sinks if not configured otherwise on the command line */
    if (u->ignore_virtual && !(sink->flags & PA_SINK_HARDWARE)) {
        pa_log_debug("Refusing to switch to virtual sink");
        return PA_HOOK_OK;
    }

    /* No default sink, nothing to move away, just set the new default */
    if (!c->default_sink) {
        pa_core_set_configured_default_sink(c, sink->name);
        return PA_HOOK_OK;
    }

    if (c->default_sink == sink) {
        pa_log_debug("%s already is the default sink", sink->name);
        return PA_HOOK_OK;
    }

    if (u->only_from_unavailable)
        if (!c->default_sink->active_port || c->default_sink->active_port->available != PA_AVAILABLE_NO) {
            pa_log_debug("Current default sink is available and module argument only_from_unavailable was set");
            return PA_HOOK_OK;
        }

    old_default_sink = c->default_sink;

    /* Actually do the switch to the new sink */
    pa_core_set_configured_default_sink(c, sink->name);

    /* Now move all old inputs over */
    if (pa_idxset_size(old_default_sink->inputs) <= 0) {
        pa_log_debug("No sink inputs to move away.");
        return PA_HOOK_OK;
    }

    PA_IDXSET_FOREACH(i, old_default_sink->inputs, idx) {
        if (i->save_sink || !PA_SINK_INPUT_IS_LINKED(i->state))
            continue;

        if (pa_sink_input_move_to(i, sink, false) < 0)
            pa_log_info("Failed to move sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), sink->name);
        else
            pa_log_info("Successfully moved sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), sink->name);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t source_put_hook_callback(pa_core *c, pa_source *source, void* userdata) {
    pa_source_output *o;
    uint32_t idx;
    pa_source *old_default_source;
    const char *s;
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(source);
    pa_assert(userdata);

    /* Don't want to run during startup or shutdown */
    if (c->state != PA_CORE_RUNNING)
        return PA_HOOK_OK;

    /* Don't switch to a monitoring source */
    if (source->monitor_of)
        return PA_HOOK_OK;

    pa_log_debug("Trying to switch to new source %s", source->name);

    /* Don't switch to any internal devices */
    s = pa_proplist_gets(source->proplist, PA_PROP_DEVICE_BUS);
    if (pa_safe_streq(s, "pci") || pa_safe_streq(s, "isa")) {
        pa_log_debug("Refusing to switch to source on %s bus", s);
        return PA_HOOK_OK;
    }

    /* Ignore virtual sources if not configured otherwise on the command line */
    if (u->ignore_virtual && !(source->flags & PA_SOURCE_HARDWARE)) {
        pa_log_debug("Refusing to switch to virtual source");
        return PA_HOOK_OK;
    }

    /* No default source, nothing to move away, just set the new default */
    if (!c->default_source) {
        pa_core_set_configured_default_source(c, source->name);
        return PA_HOOK_OK;
    }

    if (c->default_source == source) {
        pa_log_debug("%s already is the default source", source->name);
        return PA_HOOK_OK;
    }

    if (u->only_from_unavailable)
        if (!c->default_source->active_port || c->default_source->active_port->available != PA_AVAILABLE_NO) {
            pa_log_debug("Current default source is available and module argument only_from_unavailable was set");
            return PA_HOOK_OK;
        }

    old_default_source = c->default_source;

    /* Actually do the switch to the new source */
    pa_core_set_configured_default_source(c, source->name);

    /* Now move all old outputs over */
    if (pa_idxset_size(old_default_source->outputs) <= 0) {
        pa_log_debug("No source outputs to move away.");
        return PA_HOOK_OK;
    }

    PA_IDXSET_FOREACH(o, old_default_source->outputs, idx) {
        if (o->save_source || !PA_SOURCE_OUTPUT_IS_LINKED(o->state))
            continue;

        if (pa_source_output_move_to(o, source, false) < 0)
            pa_log_info("Failed to move source output %u \"%s\" to %s.", o->index,
                        pa_strnull(pa_proplist_gets(o->proplist, PA_PROP_APPLICATION_NAME)), source->name);
        else
            pa_log_info("Successfully moved source output %u \"%s\" to %s.", o->index,
                        pa_strnull(pa_proplist_gets(o->proplist, PA_PROP_APPLICATION_NAME)), source->name);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        return -1;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);

    /* A little bit later than module-rescue-streams... */
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_LATE+30, (pa_hook_cb_t) sink_put_hook_callback, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_LATE+20, (pa_hook_cb_t) source_put_hook_callback, u);

    if (pa_modargs_get_value_boolean(ma, "only_from_unavailable", &u->only_from_unavailable) < 0) {
        pa_log("Failed to get a boolean value for only_from_unavailable.");
        goto fail;
    }

    u->ignore_virtual = true;
    if (pa_modargs_get_value_boolean(ma, "ignore_virtual", &u->ignore_virtual) < 0) {
        pa_log("Failed to get a boolean value for ignore_virtual.");
        goto fail;
    }

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    pa_xfree(u);
}
