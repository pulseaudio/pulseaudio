/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>

#include "module-rescue-streams-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("When a sink/source is removed, try to move their streams to the default sink/source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    NULL,
};

struct userdata {
    pa_hook_slot *sink_slot, *source_slot;
};

static pa_hook_result_t sink_hook_callback(pa_core *c, pa_sink *sink, void* userdata) {
    pa_sink_input *i;
    pa_sink *target;

    pa_assert(c);
    pa_assert(sink);

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    if (!pa_idxset_size(sink->inputs)) {
        pa_log_debug("No sink inputs to move away.");
        return PA_HOOK_OK;
    }

    if (!(target = pa_namereg_get(c, NULL, PA_NAMEREG_SINK)) || target == sink) {
        uint32_t idx;

        for (target = pa_idxset_first(c->sinks, &idx); target; target = pa_idxset_next(c->sinks, &idx))
            if (target != sink)
                break;

        if (!target) {
            pa_log_info("No evacuation sink found.");
            return PA_HOOK_OK;
        }
    }

    while ((i = pa_idxset_first(sink->inputs, NULL))) {
        if (pa_sink_input_move_to(i, target, FALSE) < 0) {
            pa_log_warn("Failed to move sink input %u \"%s\" to %s.", i->index, pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME), target->name);
            return PA_HOOK_OK;
        }

        pa_log_info("Sucessfully moved sink input %u \"%s\" to %s.", i->index, pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME), target->name);
    }


    return PA_HOOK_OK;
}

static pa_hook_result_t source_hook_callback(pa_core *c, pa_source *source, void* userdata) {
    pa_source_output *o;
    pa_source *target;

    pa_assert(c);
    pa_assert(source);

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    if (!pa_idxset_size(source->outputs)) {
        pa_log_debug("No source outputs to move away.");
        return PA_HOOK_OK;
    }

    if (!(target = pa_namereg_get(c, NULL, PA_NAMEREG_SOURCE)) || target == source) {
        uint32_t idx;

        for (target = pa_idxset_first(c->sources, &idx); target; target = pa_idxset_next(c->sources, &idx))
            if (target != source && !target->monitor_of == !source->monitor_of)
                break;

        if (!target) {
            pa_log_info("No evacuation source found.");
            return PA_HOOK_OK;
        }
    }

    pa_assert(target != source);

    while ((o = pa_idxset_first(source->outputs, NULL))) {
        if (pa_source_output_move_to(o, target, FALSE) < 0) {
            pa_log_warn("Failed to move source output %u \"%s\" to %s.", o->index, pa_proplist_gets(o->proplist, PA_PROP_APPLICATION_NAME), target->name);
            return PA_HOOK_OK;
        }

        pa_log_info("Sucessfully moved source output %u \"%s\" to %s.", o->index, pa_proplist_gets(o->proplist, PA_PROP_APPLICATION_NAME), target->name);
    }


    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        return -1;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->sink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_hook_callback, NULL);
    u->source_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) source_hook_callback, NULL);

    pa_modargs_free(ma);
    return 0;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!m->userdata)
        return;

    u = m->userdata;
    if (u->sink_slot)
        pa_hook_slot_free(u->sink_slot);
    if (u->source_slot)
        pa_hook_slot_free(u->source_slot);

    pa_xfree(u);
}
