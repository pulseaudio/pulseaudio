/***
  This file is part of PulseAudio.

  Copyright (C) 2014 Collabora Ltd. <http://www.collabora.co.uk/>

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

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "module-allow-passthrough-symdef.h"

PA_MODULE_AUTHOR("Guillaume Desmottes");
PA_MODULE_DESCRIPTION("When a passthrough stream is requested, route all the other streams to a dummy device");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

static const char* const valid_modargs[] = {
    NULL,
};

struct userdata {
    /* (pa_sink *) -> (pa_sink *)
     * Map the 'real' muted sink to the null-sink currently being used to play
     * its streams. */
    pa_hashmap *null_sinks;

    bool moving;
};

static pa_sink *ensure_null_sink_for_sink(struct userdata *u, pa_sink *s, pa_core *c) {
    char *t;
    pa_module *m;
    pa_sink *sink;
    uint32_t idx;
    const char *name;

    sink = pa_hashmap_get(u->null_sinks, s);
    if (sink != NULL) {
        /* We already have a null-sink for this sink */
        return sink;
    }

    name = pa_proplist_gets(s->proplist, PA_PROP_MEDIA_NAME);

    t = pa_sprintf_malloc("sink_name=allow_passthrough_null_%s sink_properties='device.description=\"%s\"'",
                          name ? name : "", _("Dummy Output"));
    m = pa_module_load(c, "module-null-sink", t);
    pa_xfree(t);

    if (m == NULL)
        return NULL;

   PA_IDXSET_FOREACH(sink, c->sinks, idx) {
        if (sink->module->index == m->index) {
          pa_hashmap_put(u->null_sinks, s, sink);
          return sink;
        }
    }

   return NULL;
}

static void unload_null_sink_module_for_sink(struct userdata *u, pa_sink *s, pa_core *c) {
    pa_sink *null_sink;

    null_sink = pa_hashmap_get(u->null_sinks, s);
    if (null_sink == NULL)
        return;

    pa_module_unload_request_by_index(c, null_sink->module->index, true);

    pa_hashmap_remove(u->null_sinks, s);
}

static void move_stream(struct userdata *u, pa_sink_input *i, pa_sink *target) {
    u->moving = true;
    if (pa_sink_input_move_to(i, target, false) < 0)
        pa_log_info("Failed to move sink input %u \"%s\" to %s.", i->index,
                    pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), target->name);
    else
        pa_log_info("Successfully moved sink input %u \"%s\" to %s.", i->index,
                    pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), target->name);
    u->moving = false;
}

/* Check if @sink has any passthrough stream, ignoring @ignore */
static bool sink_has_passthrough_stream(pa_sink *sink, pa_sink_input *ignore)
{
    pa_sink_input *stream;
    uint32_t idx;

    PA_IDXSET_FOREACH(stream, sink->inputs, idx) {
        if (stream == ignore)
          continue;

        if (pa_sink_input_is_passthrough(stream))
          return true;
    }

    return false;
}

static pa_hook_result_t new_passthrough_stream(struct userdata *u, pa_core *c, pa_sink *sink, pa_sink_input *i) {
    uint32_t idx;
    pa_sink_input *stream;
    pa_sink *null_sink;

    if (sink_has_passthrough_stream(sink, i)) {
        pa_log_info("Dropping playing a passthrough stream; ignoring");
        /* PulseAudio will reject the stream itself */
        return PA_HOOK_OK;
    }

    pa_log_info("Just received a passthrough stream; pause all the others streams so it can play");

    null_sink = ensure_null_sink_for_sink(u, sink, c);
    if (null_sink == NULL)
        return PA_HOOK_OK;

    PA_IDXSET_FOREACH(stream, sink->inputs, idx) {
        /* We don't want to move the stream which just moved to the sink and trigger this re-routing */
        if (stream != i)
          move_stream(u, stream, null_sink);
    }

    return PA_HOOK_OK;
}

/* return a null sink for the new stream if it needs to be re-routed */
static pa_sink * new_normal_stream(struct userdata *u, pa_core *c, pa_sink *sink) {
    if (!sink_has_passthrough_stream(sink, NULL))
        return NULL;

    /* A passthrough stream is already playing on this sink, re-route to a null sink */
    return ensure_null_sink_for_sink(u, sink, c);
}

static pa_hook_result_t sink_input_new_cb(pa_core *core, pa_sink_input_new_data *new_data, struct userdata *u) {
    pa_sink *null_sink;

    pa_core_assert_ref(core);
    /* This is a bit of a hack, to determine whether the input stream will use
     * a passthrough stream, the sink should have been selected and a format
     * renegotiated. This can either happen by an earlier module (e.g. one
     * doing routing or other policies) and if not pulseaudio core will setup
     * the defaults after all hooks for this event have been processed.
     *
     * Unfortunately if no other module decides on sink/format before this hook
     * runs, pulse core doing it is too late, so if a sink and/or stream format
     * haven't been setup & configured just yet do so now using the same code
     * as pulsecore would use (default sink and higher priority negotiated
     * format). */
    if (!new_data->sink) {
        pa_sink *sink = pa_namereg_get(core, NULL, PA_NAMEREG_SINK);
        pa_return_val_if_fail(sink, -PA_ERR_NOENTITY);
        pa_sink_input_new_data_set_sink(new_data, sink, false);
    }

    if (!new_data->format && new_data->nego_formats && !pa_idxset_isempty(new_data->nego_formats))
      new_data->format = pa_format_info_copy(pa_idxset_first(new_data->nego_formats, NULL));

    if (pa_sink_input_new_data_is_passthrough(new_data))
        return new_passthrough_stream(u, core, new_data->sink, NULL);

    null_sink = new_normal_stream(u, core, new_data->sink);

    if (null_sink) {
        pa_log_info("Already playing a passthrough stream; re-routing new stream to the null sink");
        pa_sink_input_new_data_set_sink(new_data, null_sink, false);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t passthrough_stream_removed(struct userdata *u, pa_core *c, pa_sink_input *i) {
    uint32_t idx;
    pa_sink_input *stream;
    pa_sink *null_sink;

    pa_assert(i->sink);

    null_sink = pa_hashmap_get(u->null_sinks, i->sink);
    if (null_sink == NULL)
        return PA_HOOK_OK;

    pa_log_info("Passthrough stream removed; restore all streams");

    PA_IDXSET_FOREACH(stream, null_sink->inputs, idx) {
        move_stream(u, stream, i->sink);
    }

    unload_null_sink_module_for_sink(u, i->sink, c);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_removed(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink_input_assert_ref(i);

    if (pa_sink_input_is_passthrough(i))
      return passthrough_stream_removed(u, core, i);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    return sink_input_removed(core, i, u);
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    if (u->moving)
      return PA_HOOK_OK;

    return sink_input_removed(core, i, u);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink *null_sink;

    if (u->moving)
        return PA_HOOK_OK;

    if (pa_sink_input_is_passthrough(i))
        /* Passthrough stream has been moved to a new sink */
        return new_passthrough_stream(u, core, i->sink, i);

    null_sink = new_normal_stream(u, core, i->sink);
    if (null_sink) {
        pa_log_info("Already playing a passthrough stream; re-routing moved stream to the null sink");
        move_stream(u, i, null_sink);
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

    m->userdata = u = pa_xnew(struct userdata, 1);

    u->null_sinks = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_new_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);

    u->moving = false;

    pa_modargs_free(ma);
    return 0;
}

static void unload_all_null_sink_modules(struct userdata *u, pa_core *c) {
    void *state = NULL;
    pa_sink *null_sink;

    PA_HASHMAP_FOREACH(null_sink, u->null_sinks, state)
        pa_module_unload_request_by_index(c, null_sink->module->index, true);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (m->core->state != PA_CORE_SHUTDOWN)
        unload_all_null_sink_modules(u, m->core);

    if (u->null_sinks)
        pa_hashmap_free(u->null_sinks);

    pa_xfree(u);
}
