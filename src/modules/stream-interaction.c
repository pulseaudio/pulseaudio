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
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/volume.h>

#include <pulsecore/macro.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/modargs.h>

#include "stream-interaction.h"

struct group {
    char *name;
    pa_idxset *trigger_roles;
    pa_idxset *interaction_roles;
    pa_hashmap *interaction_state;
    pa_volume_t volume;
};

struct userdata {
    pa_core *core;
    uint32_t n_groups;
    struct group **groups;
    bool global:1;
    bool duck:1;
    bool source_trigger:1;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_unlink_slot,
        *sink_input_move_start_slot,
        *sink_input_move_finish_slot,
        *sink_input_state_changed_slot,
        *sink_input_mute_changed_slot,
        *sink_input_proplist_changed_slot,
        *source_output_put_slot,
        *source_output_unlink_slot,
        *source_output_move_start_slot,
        *source_output_move_finish_slot,
        *source_output_state_changed_slot,
        *source_output_mute_changed_slot,
        *source_output_proplist_changed_slot;
};

static inline pa_object* GET_DEVICE_FROM_STREAM(pa_object *stream) {
    return pa_sink_input_isinstance(stream) ? PA_OBJECT(PA_SINK_INPUT(stream)->sink) : PA_OBJECT(PA_SOURCE_OUTPUT(stream)->source);
}

static inline pa_proplist* GET_PROPLIST_FROM_STREAM(pa_object *stream) {
    return pa_sink_input_isinstance(stream) ? PA_SINK_INPUT(stream)->proplist : PA_SOURCE_OUTPUT(stream)->proplist;
}

static const char *get_trigger_role(struct userdata *u, pa_object *stream, struct group *g) {
    const char *role, *trigger_role;
    uint32_t role_idx;

    if (!(role = pa_proplist_gets(GET_PROPLIST_FROM_STREAM(stream), PA_PROP_MEDIA_ROLE)))
        role = "no_role";

    if (g == NULL) {
        /* get it from all groups */
        uint32_t j;
        for (j = 0; j < u->n_groups; j++) {
            PA_IDXSET_FOREACH(trigger_role, u->groups[j]->trigger_roles, role_idx) {
                if (pa_streq(role, trigger_role))
                    return trigger_role;
            }
        }
    } else {
        PA_IDXSET_FOREACH(trigger_role, g->trigger_roles, role_idx) {
            if (pa_streq(role, trigger_role))
                return trigger_role;
        }
    }

    return NULL;
}

static const char *find_trigger_stream(struct userdata *u, pa_object *device, pa_object *ignore_stream, struct group *g) {
    pa_object *j;
    uint32_t idx;
    const char *trigger_role;

    pa_assert(u);
    pa_object_assert_ref(device);

    PA_IDXSET_FOREACH(j, pa_sink_isinstance(device) ? PA_SINK(device)->inputs : PA_SOURCE(device)->outputs, idx) {
        if (j == ignore_stream)
            continue;

        if (!(trigger_role = get_trigger_role(u, PA_OBJECT(j), g)))
            continue;

        if (pa_sink_isinstance(device)) {
            if (!PA_SINK_INPUT(j)->muted &&
                PA_SINK_INPUT(j)->state != PA_SINK_INPUT_CORKED)
                return trigger_role;
        } else {
            if (!PA_SOURCE_OUTPUT(j)->muted &&
                PA_SOURCE_OUTPUT(j)->state != PA_SOURCE_OUTPUT_CORKED)
                return trigger_role;
        }
    }

    return NULL;
}

static const char *find_global_trigger_stream(struct userdata *u, pa_object *ignore_stream, struct group *g) {
    const char *trigger_role = NULL;
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;

    pa_assert(u);

    /* Find any trigger role among the sink-inputs and source-outputs. */
    PA_IDXSET_FOREACH(sink, u->core->sinks, idx)
        if ((trigger_role = find_trigger_stream(u, PA_OBJECT(sink), ignore_stream, g)))
            break;

    if (!u->source_trigger || trigger_role)
        return trigger_role;

    PA_IDXSET_FOREACH(source, u->core->sources, idx)
        if ((trigger_role = find_trigger_stream(u, PA_OBJECT(source), ignore_stream, g)))
            break;

    return trigger_role;
}

static void cork_or_duck(struct userdata *u, pa_sink_input *i, const char *interaction_role,  const char *trigger_role, bool interaction_applied, struct group *g) {

    if (u->duck && !interaction_applied) {
        pa_cvolume vol;
        vol.channels = 1;
        vol.values[0] = g->volume;

        pa_log_debug("Found a '%s' stream of '%s' that ducks a '%s' stream.", trigger_role, g->name, interaction_role);
        pa_sink_input_add_volume_factor(i, g->name, &vol);

    } else if (!u->duck) {
        pa_log_debug("Found a '%s' stream that corks/mutes a '%s' stream.", trigger_role, interaction_role);
        pa_sink_input_set_mute(i, true, false);
        pa_sink_input_send_event(i, PA_STREAM_EVENT_REQUEST_CORK, NULL);
    }
}

static void uncork_or_unduck(struct userdata *u, pa_sink_input *i, const char *interaction_role, bool corked, struct group *g) {

    if (u->duck) {
       pa_log_debug("In '%s', found a '%s' stream that should be unducked", g->name, interaction_role);
       pa_sink_input_remove_volume_factor(i, g->name);
    }
    else if (corked || i->muted) {
       pa_log_debug("Found a '%s' stream that should be uncorked/unmuted.", interaction_role);
       if (i->muted)
          pa_sink_input_set_mute(i, false, false);
       if (corked)
          pa_sink_input_send_event(i, PA_STREAM_EVENT_REQUEST_UNCORK, NULL);
    }
}

static inline void apply_interaction_to_sink(struct userdata *u, pa_sink *s, const char *new_trigger, pa_sink_input *ignore_stream, bool new_stream, struct group *g) {
    pa_sink_input *j;
    uint32_t idx, role_idx;
    const char *interaction_role;
    bool trigger = false;

    pa_assert(u);
    pa_sink_assert_ref(s);

    PA_IDXSET_FOREACH(j, s->inputs, idx) {
        bool corked, interaction_applied;
        const char *role;

        if (j == ignore_stream)
            continue;

        if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
            role = "no_role";

        PA_IDXSET_FOREACH(interaction_role, g->interaction_roles, role_idx) {
            if ((trigger = pa_streq(role, interaction_role)))
                break;
            if ((trigger = (pa_streq(interaction_role, "any_role") && !get_trigger_role(u, PA_OBJECT(j), g))))
                break;
        }
        if (!trigger)
            continue;

        /* Some applications start their streams corked, so the stream is uncorked by */
        /* the application only after sink_input_put() was called. If a new stream turns */
        /* up, act as if it was not corked. In the case of module-role-cork this will */
        /* only mute the stream because corking is reverted later by the application */
        corked = (j->state == PA_SINK_INPUT_CORKED);
        if (new_stream && corked)
            corked = false;
        interaction_applied = !!pa_hashmap_get(g->interaction_state, j);

        if (new_trigger && ((!corked && !j->muted) || u->duck)) {
            if (!interaction_applied)
                pa_hashmap_put(g->interaction_state, j, PA_INT_TO_PTR(1));

            cork_or_duck(u, j, role, new_trigger, interaction_applied, g);

        } else if (!new_trigger && interaction_applied) {
            pa_hashmap_remove(g->interaction_state, j);

            uncork_or_unduck(u, j, role, corked, g);
        }
    }
}

static void apply_interaction_global(struct userdata *u, const char *trigger_role, pa_sink_input *ignore_stream, bool new_stream, struct group *g) {
    uint32_t idx;
    pa_sink *s;

    pa_assert(u);

    PA_IDXSET_FOREACH(s, u->core->sinks, idx)
        apply_interaction_to_sink(u, s, trigger_role, ignore_stream, new_stream, g);
}

static void remove_interactions(struct userdata *u, struct group *g) {
    uint32_t idx, idx_input;
    pa_sink *s;
    pa_sink_input *j;
    bool corked;
    const char *role;

    PA_IDXSET_FOREACH(s, u->core->sinks, idx) {
        PA_IDXSET_FOREACH(j, s->inputs, idx_input) {
            if(!!pa_hashmap_get(g->interaction_state, j)) {
                corked = (j->state == PA_SINK_INPUT_CORKED);
                if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
                   role = "no_role";
                uncork_or_unduck(u, j, role, corked, g);
            }
        }
    }
}

static pa_hook_result_t process(struct userdata *u, pa_object *stream, bool create, bool new_stream) {
    const char *trigger_role;
    uint32_t j;

    pa_assert(u);
    pa_object_assert_ref(stream);

    if (!create)
        for (j = 0; j < u->n_groups; j++)
            pa_hashmap_remove(u->groups[j]->interaction_state, stream);

    if ((pa_sink_input_isinstance(stream) && !PA_SINK_INPUT(stream)->sink) ||
        (pa_source_output_isinstance(stream) && !PA_SOURCE_OUTPUT(stream)->source))
        return PA_HOOK_OK;

    if (pa_source_output_isinstance(stream)) {
        if (!u->source_trigger)
            return PA_HOOK_OK;
        /* If it is triggered from source-output with false of global option, no need to apply interaction. */
        if (!u->global)
            return PA_HOOK_OK;
    }

    for (j = 0; j < u->n_groups; j++) {
        if (u->global) {
            trigger_role = find_global_trigger_stream(u, create ? NULL : stream, u->groups[j]);
            apply_interaction_global(u, trigger_role, create ? NULL : (pa_sink_input_isinstance(stream) ? PA_SINK_INPUT(stream) : NULL), new_stream, u->groups[j]);
        } else {
            trigger_role = find_trigger_stream(u, GET_DEVICE_FROM_STREAM(stream), create ? NULL : stream, u->groups[j]);
            if (pa_sink_input_isinstance(stream))
                apply_interaction_to_sink(u, PA_SINK_INPUT(stream)->sink, trigger_role, create ? NULL : PA_SINK_INPUT(stream), new_stream, u->groups[j]);
        }
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, PA_OBJECT(i), true, true);
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink_input_assert_ref(i);

    return process(u, PA_OBJECT(i), false, false);
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, PA_OBJECT(i), false, false);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, PA_OBJECT(i), true, false);
}

static pa_hook_result_t sink_input_state_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state) && get_trigger_role(u, PA_OBJECT(i), NULL))
        return process(u, PA_OBJECT(i), true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_mute_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state) && get_trigger_role(u, PA_OBJECT(i), NULL))
        return process(u, PA_OBJECT(i), true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_proplist_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state))
        return process(u, PA_OBJECT(i), true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_put_cb(pa_core *core, pa_source_output *o, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_source_output_assert_ref(o);

    return process(u, PA_OBJECT(o), true, true);
}

static pa_hook_result_t source_output_unlink_cb(pa_core *core, pa_source_output *o, struct userdata *u) {
    pa_source_output_assert_ref(o);

    return process(u, PA_OBJECT(o), false, false);
}

static pa_hook_result_t source_output_move_start_cb(pa_core *core, pa_source_output *o, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_source_output_assert_ref(o);

    return process(u, PA_OBJECT(o), false, false);
}

static pa_hook_result_t source_output_move_finish_cb(pa_core *core, pa_source_output *o, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_source_output_assert_ref(o);

    return process(u, PA_OBJECT(o), true, false);
}

static pa_hook_result_t source_output_state_changed_cb(pa_core *core, pa_source_output *o, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_source_output_assert_ref(o);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state) && get_trigger_role(u, PA_OBJECT(o), NULL))
        return process(u, PA_OBJECT(o), true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_mute_changed_cb(pa_core *core, pa_source_output*o, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_source_output_assert_ref(o);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state) && get_trigger_role(u, PA_OBJECT(o), NULL))
        return process(u, PA_OBJECT(o), true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_proplist_changed_cb(pa_core *core, pa_source_output *o, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_source_output_assert_ref(o);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state))
        return process(u, PA_OBJECT(o), true, false);

    return PA_HOOK_OK;
}

int pa_stream_interaction_init(pa_module *m, const char* const v_modargs[]) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *roles;
    char *roles_in_group = NULL;
    bool global = false;
    bool source_trigger = false;
    uint32_t i = 0;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, v_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);

    u->core = m->core;

    u->duck = false;
    if (pa_streq(m->name, "module-role-ducking"))
        u->duck = true;

    u->n_groups = 1;

    if (u->duck) {
        const char *volumes;
        uint32_t group_count_tr = 0;
        uint32_t group_count_du = 0;
        uint32_t group_count_vol = 0;

        roles = pa_modargs_get_value(ma, "trigger_roles", NULL);
        if (roles) {
            const char *split_state = NULL;
            char *n = NULL;
            while ((n = pa_split(roles, "/", &split_state))) {
                group_count_tr++;
                pa_xfree(n);
            }
        }
        roles = pa_modargs_get_value(ma, "ducking_roles", NULL);
        if (roles) {
            const char *split_state = NULL;
            char *n = NULL;
            while ((n = pa_split(roles, "/", &split_state))) {
                group_count_du++;
                pa_xfree(n);
            }
        }
        volumes = pa_modargs_get_value(ma, "volume", NULL);
        if (volumes) {
            const char *split_state = NULL;
            char *n = NULL;
            while ((n = pa_split(volumes, "/", &split_state))) {
                group_count_vol++;
                pa_xfree(n);
            }
        }

        if ((group_count_tr > 1 || group_count_du > 1 || group_count_vol > 1) &&
            ((group_count_tr != group_count_du) || (group_count_tr != group_count_vol))) {
            pa_log("Invalid number of groups");
            goto fail;
        }

        if (group_count_tr > 0)
            u->n_groups = group_count_tr;
    }

    u->groups = pa_xnew0(struct group*, u->n_groups);
    for (i = 0; i < u->n_groups; i++) {
        u->groups[i] = pa_xnew0(struct group, 1);
        u->groups[i]->trigger_roles = pa_idxset_new(NULL, NULL);
        u->groups[i]->interaction_roles = pa_idxset_new(NULL, NULL);
        u->groups[i]->interaction_state = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
        if (u->duck)
            u->groups[i]->name = pa_sprintf_malloc("ducking_group_%u", i);
    }

    roles = pa_modargs_get_value(ma, "trigger_roles", NULL);
    if (roles) {
        const char *group_split_state = NULL;
        i = 0;

        while ((roles_in_group = pa_split(roles, "/", &group_split_state))) {
            if (roles_in_group[0] != '\0') {
                const char *split_state = NULL;
                char *n = NULL;
                while ((n = pa_split(roles_in_group, ",", &split_state))) {
                    if (n[0] != '\0')
                        pa_idxset_put(u->groups[i]->trigger_roles, n, NULL);
                    else {
                        pa_log("empty trigger role");
                        pa_xfree(n);
                        goto fail;
                    }
                }
                i++;
            } else {
                pa_log("empty trigger roles");
                goto fail;
            }

            pa_xfree(roles_in_group);
        }
    }
    if (pa_idxset_isempty(u->groups[0]->trigger_roles)) {
        pa_log_debug("Using role 'phone' as trigger role.");
        pa_idxset_put(u->groups[0]->trigger_roles, pa_xstrdup("phone"), NULL);
    }

    roles = pa_modargs_get_value(ma, u->duck ? "ducking_roles" : "cork_roles", NULL);
    if (roles) {
        const char *group_split_state = NULL;
        i = 0;

        while ((roles_in_group = pa_split(roles, "/", &group_split_state))) {
            if (roles_in_group[0] != '\0') {
                const char *split_state = NULL;
                char *n = NULL;
                while ((n = pa_split(roles_in_group, ",", &split_state))) {
                    if (n[0] != '\0')
                        pa_idxset_put(u->groups[i]->interaction_roles, n, NULL);
                    else {
                        pa_log("empty ducking role");
                        pa_xfree(n);
                        goto fail;
                     }
                }
                i++;
            } else {
                pa_log("empty ducking roles");
                goto fail;
            }

            pa_xfree(roles_in_group);
        }
    }
    if (pa_idxset_isempty(u->groups[0]->interaction_roles)) {
        pa_log_debug("Using roles 'music' and 'video' as %s roles.", u->duck ? "ducking" : "cork");
        pa_idxset_put(u->groups[0]->interaction_roles, pa_xstrdup("music"), NULL);
        pa_idxset_put(u->groups[0]->interaction_roles, pa_xstrdup("video"), NULL);
    }

    if (u->duck) {
        const char *volumes;
        u->groups[0]->volume = pa_sw_volume_from_dB(-20);
        if ((volumes = pa_modargs_get_value(ma, "volume", NULL))) {
            const char *group_split_state = NULL;
            char *n = NULL;
            i = 0;
            while ((n = pa_split(volumes, "/", &group_split_state))) {
                if (n[0] != '\0') {
                    if (pa_parse_volume(n, &(u->groups[i++]->volume)) < 0) {
                        pa_log("Failed to parse volume");
                        pa_xfree(n);
                        goto fail;
                    }
                } else {
                    pa_log("empty volume");
                    pa_xfree(n);
                    goto fail;
                }
                pa_xfree(n);
            }
        }
    }

    if (pa_modargs_get_value_boolean(ma, "global", &global) < 0) {
        pa_log("Invalid boolean parameter: global");
        goto fail;
    }
    u->global = global;

    if (pa_modargs_get_value_boolean(ma, "use_source_trigger", &source_trigger) < 0) {
        pa_log("Invalid boolean parameter: use_source_trigger");
        goto fail;
    }
    u->source_trigger = source_trigger;

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);
    u->sink_input_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_state_changed_cb, u);
    u->sink_input_mute_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MUTE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_mute_changed_cb, u);
    u->sink_input_proplist_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_proplist_changed_cb, u);
    u->source_output_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) source_output_put_cb, u);
    u->source_output_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) source_output_unlink_cb, u);
    u->source_output_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) source_output_move_start_cb, u);
    u->source_output_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) source_output_move_finish_cb, u);
    u->source_output_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) source_output_state_changed_cb, u);
    u->source_output_mute_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MUTE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) source_output_mute_changed_cb, u);
    u->source_output_proplist_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) source_output_proplist_changed_cb, u);

    pa_modargs_free(ma);

    return 0;

fail:
    pa_stream_interaction_done(m);

    if (ma)
        pa_modargs_free(ma);
    if (roles_in_group)
        pa_xfree(roles_in_group);

    return -1;

}

void pa_stream_interaction_done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->groups) {
        uint32_t j;
        for (j = 0; j < u->n_groups; j++) {
            remove_interactions(u, u->groups[j]);
            pa_idxset_free(u->groups[j]->trigger_roles, pa_xfree);
            pa_idxset_free(u->groups[j]->interaction_roles, pa_xfree);
            pa_hashmap_free(u->groups[j]->interaction_state);
            if (u->duck)
                pa_xfree(u->groups[j]->name);
            pa_xfree(u->groups[j]);
        }
        pa_xfree(u->groups);
    }

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_input_move_start_slot)
        pa_hook_slot_free(u->sink_input_move_start_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);
    if (u->sink_input_mute_changed_slot)
        pa_hook_slot_free(u->sink_input_mute_changed_slot);
    if (u->sink_input_proplist_changed_slot)
        pa_hook_slot_free(u->sink_input_proplist_changed_slot);
    if (u->source_output_put_slot)
        pa_hook_slot_free(u->source_output_put_slot);
    if (u->source_output_unlink_slot)
        pa_hook_slot_free(u->source_output_unlink_slot);
    if (u->source_output_move_start_slot)
        pa_hook_slot_free(u->source_output_move_start_slot);
    if (u->source_output_move_finish_slot)
        pa_hook_slot_free(u->source_output_move_finish_slot);
    if (u->source_output_state_changed_slot)
        pa_hook_slot_free(u->source_output_state_changed_slot);
    if (u->source_output_mute_changed_slot)
        pa_hook_slot_free(u->source_output_mute_changed_slot);
    if (u->source_output_proplist_changed_slot)
        pa_hook_slot_free(u->source_output_proplist_changed_slot);

    pa_xfree(u);

}
