/***
  This file is part of PulseAudio.

  Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies).
  Copyright 2004-2006, 2008 Lennart Poettering

  Contact: Marc-Andre Lureau <marc-andre.lureau@nokia.com>

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
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "module-flat-volume-symdef.h"

PA_MODULE_AUTHOR("Marc-Andre Lureau");
PA_MODULE_DESCRIPTION("Flat volume");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE("");

struct userdata {
    pa_subscription *subscription;
    pa_hook_slot *sink_input_set_volume_hook_slot;
    pa_hook_slot *sink_input_fixate_hook_slot;
};

static void process_input_volume_change(
        pa_cvolume *dest_volume,
        const pa_cvolume *dest_virtual_volume,
        pa_channel_map *dest_channel_map,
        pa_sink_input *this,
        pa_sink *sink) {

    pa_sink_input *i;
    uint32_t idx;
    pa_cvolume max_volume, sink_volume;

    pa_assert(dest_volume);
    pa_assert(dest_virtual_volume);
    pa_assert(dest_channel_map);
    pa_assert(sink);

    if (!(sink->flags & PA_SINK_DECIBEL_VOLUME))
        return;

    pa_log_debug("Sink input volume changed");

    max_volume = *dest_virtual_volume;
    pa_cvolume_remap(&max_volume, dest_channel_map, &sink->channel_map);

    for (i = PA_SINK_INPUT(pa_idxset_first(sink->inputs, &idx)); i; i = PA_SINK_INPUT(pa_idxset_next(sink->inputs, &idx))) {
        /* skip this sink-input if we are processing a volume change request */
        if (this && this == i)
            continue;

        if (pa_cvolume_max(&i->virtual_volume) > pa_cvolume_max(&max_volume)) {
            max_volume = i->virtual_volume;
            pa_cvolume_remap(&max_volume, &i->channel_map, &sink->channel_map);
        }
    }

    /* Set the master volume, and normalize inputs */
    if (!pa_cvolume_equal(&max_volume, &sink->volume)) {

        pa_sink_set_volume(sink, &max_volume);

        pa_log_debug("sink = %.2f (changed)", (double)pa_cvolume_avg(&sink->volume)/PA_VOLUME_NORM);

        /* Now, normalize each of the internal volume (client sink-input volume / sink master volume) */
        for (i = PA_SINK_INPUT(pa_idxset_first(sink->inputs, &idx)); i; i = PA_SINK_INPUT(pa_idxset_next(sink->inputs, &idx))) {
            /* skip this sink-input if we are processing a volume change request */
            if (this && this == i)
                continue;

            sink_volume = max_volume;
            pa_cvolume_remap(&sink_volume, &sink->channel_map, &i->channel_map);
            pa_sw_cvolume_divide(&i->volume, &i->virtual_volume, &sink_volume);
            pa_log_debug("sink input { id = %d, flat = %.2f, true = %.2f }",
                         i->index,
                         (double)pa_cvolume_avg(&i->virtual_volume)/PA_VOLUME_NORM,
                         (double)pa_cvolume_avg(&i->volume)/PA_VOLUME_NORM);
            pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, &i->volume, 1), 0, NULL, pa_xfree);
        }
    } else
        pa_log_debug("sink = %.2f", (double)pa_cvolume_avg(&sink->volume)/PA_VOLUME_NORM);

    /* and this one */

    sink_volume = max_volume;
    pa_cvolume_remap(&sink_volume, &sink->channel_map, dest_channel_map);
    pa_sw_cvolume_divide(dest_volume, dest_virtual_volume, &sink_volume);
    pa_log_debug("caller sink input: { id = %d, flat = %.2f, true = %.2f }",
                 this ? (int)this->index : -1,
                 (double)pa_cvolume_avg(dest_virtual_volume)/PA_VOLUME_NORM,
                 (double)pa_cvolume_avg(dest_volume)/PA_VOLUME_NORM);
}

static pa_hook_result_t sink_input_set_volume_hook_callback(pa_core *c, pa_sink_input_set_volume_data *this, struct userdata *u) {
    pa_assert(this);
    pa_assert(this->sink_input);

    process_input_volume_change(&this->volume, &this->virtual_volume, &this->sink_input->channel_map,
                                this->sink_input, this->sink_input->sink);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_fixate_hook_callback(pa_core *core, pa_sink_input_new_data *this, struct userdata *u) {
    pa_assert(this);
    pa_assert(this->sink);

    process_input_volume_change(&this->volume, &this->virtual_volume, &this->channel_map,
                                NULL, this->sink);

    return PA_HOOK_OK;
}

static void subscribe_callback(pa_core *core, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    pa_sink *sink;
    pa_sink_input *i;
    uint32_t iidx;
    pa_cvolume sink_volume;

    pa_assert(core);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if (!(sink = pa_idxset_get_by_index(core->sinks, idx)))
        return;

    if (!(sink->flags & PA_SINK_DECIBEL_VOLUME))
        return;

    pa_log_debug("Sink volume changed");
    pa_log_debug("sink = %.2f", (double)pa_cvolume_avg(pa_sink_get_volume(sink, FALSE)) / PA_VOLUME_NORM);

    sink_volume = *pa_sink_get_volume(sink, FALSE);

    for (i = PA_SINK_INPUT(pa_idxset_first(sink->inputs, &iidx)); i; i = PA_SINK_INPUT(pa_idxset_next(sink->inputs, &iidx))) {
        pa_cvolume si_volume;

        si_volume = sink_volume;
        pa_cvolume_remap(&si_volume, &sink->channel_map, &i->channel_map);
        pa_sw_cvolume_multiply(&i->virtual_volume, &i->volume, &si_volume);
        pa_log_debug("sink input = { id = %d, flat = %.2f, true = %.2f }",
                     i->index,
                     (double)pa_cvolume_avg(&i->virtual_volume)/PA_VOLUME_NORM,
                     (double)pa_cvolume_avg(&i->volume)/PA_VOLUME_NORM);
        pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    }
}

int pa__init(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    u = pa_xnew(struct userdata, 1);
    m->userdata = u;

    u->sink_input_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_fixate_hook_callback, u);
    u->sink_input_set_volume_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_SET_VOLUME], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_set_volume_hook_callback, u);

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK, subscribe_callback, u);

    return 0;
}

void pa__done(pa_module*m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
      pa_subscription_free(u->subscription);

    if (u->sink_input_set_volume_hook_slot)
        pa_hook_slot_free(u->sink_input_set_volume_hook_slot);
    if (u->sink_input_fixate_hook_slot)
        pa_hook_slot_free(u->sink_input_fixate_hook_slot);

    pa_xfree(u);
}
