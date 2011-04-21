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

#include <pulsecore/macro.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/modargs.h>

#include "module-filter-heuristics-symdef.h"

#define PA_PROP_FILTER_APPLY_MOVING "filter.apply.moving"
#define PA_PROP_FILTER_HEURISTICS "filter.heuristics"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("Detect when various filters are desirable");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_move_finish_slot;
};

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i) {
    const char *want, *sink_role, *si_role;

    /* If the stream already specifies what it must have, then let it be. */
    if (!pa_proplist_gets(i->proplist, PA_PROP_FILTER_HEURISTICS) && pa_proplist_gets(i->proplist, PA_PROP_FILTER_APPLY))
        return PA_HOOK_OK;

    want = pa_proplist_gets(i->proplist, PA_PROP_FILTER_WANT);
    if (!want) {
        /* This is a phone stream, maybe we want echo cancellation */
        if ((si_role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE)) && pa_streq(si_role, "phone"))
            want = "echo-cancel";
    }

    /* On phone sinks, make sure we're not applying echo cancellation */
    if ((sink_role = pa_proplist_gets(i->sink->proplist, PA_PROP_DEVICE_INTENDED_ROLES)) && strstr(sink_role, "phone")) {
        const char *apply = pa_proplist_gets(i->proplist, PA_PROP_FILTER_APPLY);

        if (apply && pa_streq(apply, "echo-cancel")) {
            pa_proplist_unset(i->proplist, PA_PROP_FILTER_APPLY);
            pa_proplist_unset(i->proplist, PA_PROP_FILTER_HEURISTICS);
        }

        return PA_HOOK_OK;
    }

    if (want) {
        /* There's a filter that we want, ask module-filter-apply to apply it, and remember that we're managing filter.apply */
        pa_proplist_sets(i->proplist, PA_PROP_FILTER_APPLY, want);
        pa_proplist_sets(i->proplist, PA_PROP_FILTER_HEURISTICS, "1");
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);
    pa_assert(u);

    return process(u, i);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);
    pa_assert(u);

    /* module-filter-apply triggered this move, ignore */
    if (pa_proplist_gets(i->proplist, PA_PROP_FILTER_APPLY_MOVING))
        return PA_HOOK_OK;

    return process(u, i);
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);

    u->core = m->core;

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE-1, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE-1, (pa_hook_cb_t) sink_input_move_finish_cb, u);

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

    pa_xfree(u);

}
