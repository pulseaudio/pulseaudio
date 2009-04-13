/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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

#include "module-cork-music-on-phone-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Mute or cork music while a phone stream exists");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_hashmap *cork_state;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_unlink_slot,
        *sink_input_move_start_slot,
        *sink_input_move_finish_slot;
};

static pa_bool_t shall_cork(pa_sink *s, pa_sink_input *ignore) {
    pa_sink_input *j;
    uint32_t idx;
    pa_sink_assert_ref(s);

    for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {
        const char *role;

        if (j == ignore)
            continue;

        if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
            continue;

        if (pa_streq(role, "phone"))
            return TRUE;
    }

    return FALSE;
}

static void apply_cork(struct userdata *u, pa_sink *s, pa_sink_input *ignore, pa_bool_t cork) {
    pa_sink_input *j;
    uint32_t idx;

    pa_assert(u);
    pa_sink_assert_ref(s);

    for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {
        pa_bool_t corked;
        const char *role;

        if (j == ignore)
            continue;

        if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
            continue;

        if (!pa_streq(role, "video") &&
            !pa_streq(role, "music"))
            continue;

        corked = !!pa_hashmap_get(u->cork_state, j);

        if (cork && !corked) {
            pa_hashmap_put(u->cork_state, j, PA_INT_TO_PTR(1));
            pa_sink_input_set_mute(j, TRUE, FALSE);
            pa_sink_input_send_event(j, PA_STREAM_EVENT_REQUEST_CORK, NULL);
        } else if (!cork) {
            pa_hashmap_remove(u->cork_state, j);

            if (corked) {
                pa_sink_input_set_mute(j, FALSE, FALSE);
                pa_sink_input_send_event(j, PA_STREAM_EVENT_REQUEST_UNCORK, NULL);
            }
        }
    }
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i, pa_bool_t create) {
    pa_bool_t cork = FALSE;
    const char *role;

    pa_assert(u);
    pa_sink_input_assert_ref(i);

    if (!create)
        pa_hashmap_remove(u->cork_state, i);

    if (!(role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE)))
        return PA_HOOK_OK;

    if (!pa_streq(role, "phone") &&
        !pa_streq(role, "music") &&
        !pa_streq(role, "video"))
        return PA_HOOK_OK;

    cork = shall_cork(i->sink, create ? NULL : i);
    apply_cork(u, i->sink, create ? NULL : i, cork);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, TRUE);
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink_input_assert_ref(i);

    return process(u, i, FALSE);
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, FALSE);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, TRUE);
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
    u->cork_state = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return  -1;


}

void pa__done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_input_move_start_slot)
        pa_hook_slot_free(u->sink_input_move_start_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);

    if (u->cork_state)
        pa_hashmap_free(u->cork_state, NULL, NULL);

    pa_xfree(u);

}
