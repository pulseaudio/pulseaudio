/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2011 Canonical Ltd

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

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/device-port.h>
#include <pulsecore/hashmap.h>

PA_MODULE_AUTHOR("David Henningsson");
PA_MODULE_DESCRIPTION("Switches ports and profiles when devices are plugged/unplugged");
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_VERSION(PACKAGE_VERSION);

struct card_info {
    struct userdata *userdata;
    pa_card *card;

    /* We need to cache the active profile, because we want to compare the old
     * and new profiles in the PROFILE_CHANGED hook. Without this we'd only
     * have access to the new profile. */
    pa_card_profile *active_profile;
};

struct userdata {
    pa_hashmap *card_infos; /* pa_card -> struct card_info */
};

static void card_info_new(struct userdata *u, pa_card *card) {
    struct card_info *info;

    info = pa_xnew0(struct card_info, 1);
    info->userdata = u;
    info->card = card;
    info->active_profile = card->active_profile;

    pa_hashmap_put(u->card_infos, card, info);
}

static void card_info_free(struct card_info *info) {
    pa_hashmap_remove(info->userdata->card_infos, info->card);
    pa_xfree(info);
}

static bool profile_good_for_output(pa_card_profile *profile, pa_device_port *port) {
    pa_card *card;
    pa_sink *sink;
    uint32_t idx;

    pa_assert(profile);

    card = profile->card;

    if (!pa_safe_streq(card->active_profile->input_name, profile->input_name))
        return false;

    if (card->active_profile->n_sources != profile->n_sources)
        return false;

    if (card->active_profile->max_source_channels != profile->max_source_channels)
        return false;

    if (port == card->preferred_output_port)
        return true;

    PA_IDXSET_FOREACH(sink, card->sinks, idx) {
        if (!sink->active_port)
            continue;

        if ((sink->active_port->available != PA_AVAILABLE_NO) && (sink->active_port->priority >= port->priority))
            return false;
    }

    return true;
}

static bool profile_good_for_input(pa_card_profile *profile, pa_device_port *port) {
    pa_card *card;
    pa_source *source;
    uint32_t idx;

    pa_assert(profile);

    card = profile->card;

    if (!pa_safe_streq(card->active_profile->output_name, profile->output_name))
        return false;

    if (card->active_profile->n_sinks != profile->n_sinks)
        return false;

    if (card->active_profile->max_sink_channels != profile->max_sink_channels)
        return false;

    if (port == card->preferred_input_port)
        return true;

    PA_IDXSET_FOREACH(source, card->sources, idx) {
        if (!source->active_port)
            continue;

        if ((source->active_port->available != PA_AVAILABLE_NO) && (source->active_port->priority >= port->priority))
            return false;
    }

    return true;
}

static int try_to_switch_profile(pa_device_port *port) {
    pa_card_profile *best_profile = NULL, *profile;
    void *state;
    unsigned best_prio = 0;

    pa_log_debug("Finding best profile for port %s, preferred = %s",
                 port->name, pa_strnull(port->preferred_profile));

    PA_HASHMAP_FOREACH(profile, port->profiles, state) {
        bool good = false;
        const char *name;
        unsigned prio = profile->priority;

        /* We make a best effort to keep other direction unchanged */
        switch (port->direction) {
            case PA_DIRECTION_OUTPUT:
                name = profile->output_name;
                good = profile_good_for_output(profile, port);
                break;

            case PA_DIRECTION_INPUT:
                name = profile->input_name;
                good = profile_good_for_input(profile, port);
                break;
        }

        if (!good)
            continue;

        /* Give a high bonus in case this is the preferred profile */
        if (pa_safe_streq(name ? name : profile->name, port->preferred_profile))
            prio += 1000000;

        if (best_profile && best_prio >= prio)
            continue;

        best_profile = profile;
        best_prio = prio;
    }

    if (!best_profile) {
        pa_log_debug("No suitable profile found");
        return -1;
    }

    if (pa_card_set_profile(port->card, best_profile, false) != 0) {
        pa_log_debug("Could not set profile %s", best_profile->name);
        return -1;
    }

    return 0;
}

struct port_pointers {
    pa_device_port *port;
    pa_sink *sink;
    pa_source *source;
    bool is_possible_profile_active;
    bool is_preferred_profile_active;
    bool is_port_active;
};

static const char* profile_name_for_dir(pa_card_profile *cp, pa_direction_t dir) {
    if (dir == PA_DIRECTION_OUTPUT && cp->output_name)
        return cp->output_name;
    if (dir == PA_DIRECTION_INPUT && cp->input_name)
        return cp->input_name;
    return cp->name;
}

static struct port_pointers find_port_pointers(pa_device_port *port) {
    struct port_pointers pp = { .port = port };
    uint32_t state;
    pa_card *card;

    pa_assert(port);
    pa_assert_se(card = port->card);

    switch (port->direction) {
        case PA_DIRECTION_OUTPUT:
            PA_IDXSET_FOREACH(pp.sink, card->sinks, state)
                if (port == pa_hashmap_get(pp.sink->ports, port->name))
                    break;
            break;

        case PA_DIRECTION_INPUT:
            PA_IDXSET_FOREACH(pp.source, card->sources, state)
                if (port == pa_hashmap_get(pp.source->ports, port->name))
                    break;
            break;
    }

    pp.is_possible_profile_active =
        card->active_profile == pa_hashmap_get(port->profiles, card->active_profile->name);
    pp.is_preferred_profile_active = pp.is_possible_profile_active && (!port->preferred_profile ||
        pa_safe_streq(port->preferred_profile, profile_name_for_dir(card->active_profile, port->direction)));
    pp.is_port_active = (pp.sink && pp.sink->active_port == port) || (pp.source && pp.source->active_port == port);

    return pp;
}

/* Switches to a port, switching profiles if necessary or preferred */
static void switch_to_port(pa_device_port *port, struct port_pointers pp) {
    if (pp.is_port_active)
        return; /* Already selected */

    pa_log_debug("Trying to switch to port %s", port->name);
    if (!pp.is_preferred_profile_active) {
        if (try_to_switch_profile(port) < 0) {
            if (!pp.is_possible_profile_active)
                return;
        }
        else
            /* Now that profile has changed, our sink and source pointers must be updated */
            pp = find_port_pointers(port);
    }

    if (pp.source)
        pa_source_set_port(pp.source, port->name, false);
    if (pp.sink)
        pa_sink_set_port(pp.sink, port->name, false);
}

/* Switches away from a port, switching profiles if necessary or preferred */
static void switch_from_port(pa_device_port *port, struct port_pointers pp) {
    pa_device_port *p, *best_port = NULL;
    void *state;

    if (!pp.is_port_active)
        return; /* Already deselected */

    /* Try to find a good enough port to switch to */
    PA_HASHMAP_FOREACH(p, port->card->ports, state) {
        if (p == port)
            continue;

        if (p->available == PA_AVAILABLE_NO)
            continue;

        if (p->direction != port->direction)
            continue;

        if (!best_port || best_port->priority < p->priority)
           best_port = p;
    }

    pa_log_debug("Trying to switch away from port %s, found %s", port->name, best_port ? best_port->name : "no better option");

    /* If there is no available port to switch to we need check if the active
     * profile is still available in the
     * PA_CORE_HOOK_CARD_PROFILE_AVAILABLE_CHANGED callback, as at this point
     * the profile availability hasn't been updated yet. */
    if (best_port) {
        struct port_pointers best_pp = find_port_pointers(best_port);
        switch_to_port(best_port, best_pp);
    }
}


static pa_hook_result_t port_available_hook_callback(pa_core *c, pa_device_port *port, void* userdata) {
    struct port_pointers pp = find_port_pointers(port);

    if (!port->card) {
        pa_log_warn("Port %s does not have a card", port->name);
        return PA_HOOK_OK;
    }

    /* Our profile switching logic caused trouble with bluetooth headsets (see
     * https://bugs.freedesktop.org/show_bug.cgi?id=107044) and
     * module-bluetooth-policy takes care of automatic profile switching
     * anyway, so we ignore bluetooth cards in
     * module-switch-on-port-available. */
    if (pa_safe_streq(pa_proplist_gets(port->card->proplist, PA_PROP_DEVICE_BUS), "bluetooth"))
        return PA_HOOK_OK;

    switch (port->available) {
    case PA_AVAILABLE_UNKNOWN:
        /* If a port availability became unknown, let's see if it's part of
         * some availability group. If it is, it is likely to be a headphone
         * jack that does not have impedance sensing to detect whether what was
         * plugged in was a headphone, headset or microphone. In desktop
         * environments that support it, this will trigger a user choice to
         * select what kind of device was plugged in. However, let's switch to
         * the headphone port at least, so that we have don't break
         * functionality for setups that can't trigger this kind of
         * interaction.
         *
         * For headset or microphone, if they are part of some availability group
         * and they become unknown from off, it needs to check if their source is
         * unlinked or not, if their source is unlinked, let switch_to_port()
         * process them, then with the running of pa_card_set_profile(), their
         * source will be created, otherwise the headset or microphone can't be used
         * to record sound since there is no source for these 2 ports. This issue
         * is observed on Dell machines which have multi-function audio jack but no
         * internal mic.
         *
         * We should make this configurable so that users can optionally
         * override the default to a headset or mic. */

        /* Not part of a group of ports, so likely not a combination port */
        if (!port->availability_group) {
            pa_log_debug("Not switching to port %s, its availability is unknown and it's not in any availability group.", port->name);
            break;
        }

        /* Switch the headphone port, the input ports without source and the
         * input ports their source->active_port is part of a group of ports.
         */
        if (port->direction == PA_DIRECTION_INPUT && pp.source && !pp.source->active_port->availability_group) {
            pa_log_debug("Not switching to input port %s, its availability is unknown.", port->name);
            break;
        }

        switch_to_port(port, pp);
        break;

    case PA_AVAILABLE_YES:
        switch_to_port(port, pp);
        break;
    case PA_AVAILABLE_NO:
        switch_from_port(port, pp);
        break;
    default:
        break;
    }

    return PA_HOOK_OK;
}

static pa_card_profile *find_best_profile(pa_card *card) {
    pa_card_profile *profile, *best_profile;
    void *state;

    pa_assert(card);
    best_profile = pa_hashmap_get(card->profiles, "off");

    PA_HASHMAP_FOREACH(profile, card->profiles, state) {
        if (profile->available == PA_AVAILABLE_NO)
            continue;

        if (profile->priority > best_profile->priority)
            best_profile = profile;
    }

    return best_profile;
}

static pa_hook_result_t card_profile_available_hook_callback(pa_core *c, pa_card_profile *profile, struct userdata *u) {
    pa_card *card;

    pa_assert(profile);
    pa_assert_se(card = profile->card);

    if (profile->available != PA_AVAILABLE_NO)
        return PA_HOOK_OK;

    if (!pa_streq(profile->name, card->active_profile->name))
        return PA_HOOK_OK;

    pa_log_debug("Active profile %s on card %s became unavailable, switching to another profile", profile->name, card->name);
    pa_card_set_profile(card, find_best_profile(card), false);

    return PA_HOOK_OK;

}

static void handle_all_unavailable(pa_core *core) {
    pa_card *card;
    uint32_t state;

    PA_IDXSET_FOREACH(card, core->cards, state) {
        pa_device_port *port;
        void *state2;

        PA_HASHMAP_FOREACH(port, card->ports, state2) {
            if (port->available == PA_AVAILABLE_NO)
                port_available_hook_callback(core, port, NULL);
        }
    }
}

static pa_device_port *new_sink_source(pa_hashmap *ports, const char *name) {

    void *state;
    pa_device_port *i, *p = NULL;

    if (!ports)
        return NULL;
    if (name)
        p = pa_hashmap_get(ports, name);
    if (!p)
        PA_HASHMAP_FOREACH(i, ports, state)
            if (!p || i->priority > p->priority)
                p = i;
    if (!p)
        return NULL;
    if (p->available != PA_AVAILABLE_NO)
        return NULL;

    pa_assert_se(p = pa_device_port_find_best(ports));
    return p;
}

static pa_hook_result_t sink_new_hook_callback(pa_core *c, pa_sink_new_data *new_data, void *u) {

    pa_device_port *p = new_sink_source(new_data->ports, new_data->active_port);

    if (p) {
        pa_log_debug("Switching initial port for sink '%s' to '%s'", new_data->name, p->name);
        pa_sink_new_data_set_port(new_data, p->name);
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t source_new_hook_callback(pa_core *c, pa_source_new_data *new_data, void *u) {

    pa_device_port *p = new_sink_source(new_data->ports, new_data->active_port);

    if (p) {
        pa_log_debug("Switching initial port for source '%s' to '%s'", new_data->name, p->name);
        pa_source_new_data_set_port(new_data, p->name);
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t card_put_hook_callback(pa_core *core, pa_card *card, struct userdata *u) {
    card_info_new(u, card);

    return PA_HOOK_OK;
}

static pa_hook_result_t card_unlink_hook_callback(pa_core *core, pa_card *card, struct userdata *u) {
    card_info_free(pa_hashmap_get(u->card_infos, card));

    return PA_HOOK_OK;
}

static void update_preferred_input_port(pa_card *card, pa_card_profile *old_profile, pa_card_profile *new_profile) {
    pa_source *source;

    /* If the profile change didn't affect input, it doesn't indicate change in
     * the user's input port preference. */
    if (pa_safe_streq(old_profile->input_name, new_profile->input_name))
        return;

    /* If there are more than one source, we don't know which of those the user
     * prefers. If there are no sources, then the user doesn't seem to care
     * about input at all. */
    if (pa_idxset_size(card->sources) != 1) {
        pa_card_set_preferred_port(card, PA_DIRECTION_INPUT, NULL);
        return;
    }

    /* If the profile change modified the set of sinks, then it's unclear
     * whether the user wanted to activate some specific input port, or was the
     * input change only a side effect of activating some output. If the new
     * profile contains no sinks, though, then we know the user only cares
     * about input. */
    if (pa_idxset_size(card->sinks) > 0 && !pa_safe_streq(old_profile->output_name, new_profile->output_name)) {
        pa_card_set_preferred_port(card, PA_DIRECTION_INPUT, NULL);
        return;
    }

    source = pa_idxset_first(card->sources, NULL);

    /* We know the user wanted to activate this source. The user might not have
     * wanted to activate the port that was selected by default, but if that's
     * the case, the user will change the port manually, and we'll update the
     * port preference at that time. If no port change occurs, we can assume
     * that the user likes the port that is now active. */
    pa_card_set_preferred_port(card, PA_DIRECTION_INPUT, source->active_port);
}

static void update_preferred_output_port(pa_card *card, pa_card_profile *old_profile, pa_card_profile *new_profile) {
    pa_sink *sink;

    /* If the profile change didn't affect output, it doesn't indicate change in
     * the user's output port preference. */
    if (pa_safe_streq(old_profile->output_name, new_profile->output_name))
        return;

    /* If there are more than one sink, we don't know which of those the user
     * prefers. If there are no sinks, then the user doesn't seem to care about
     * output at all. */
    if (pa_idxset_size(card->sinks) != 1) {
        pa_card_set_preferred_port(card, PA_DIRECTION_OUTPUT, NULL);
        return;
    }

    /* If the profile change modified the set of sources, then it's unclear
     * whether the user wanted to activate some specific output port, or was
     * the output change only a side effect of activating some input. If the
     * new profile contains no sources, though, then we know the user only
     * cares about output. */
    if (pa_idxset_size(card->sources) > 0 && !pa_safe_streq(old_profile->input_name, new_profile->input_name)) {
        pa_card_set_preferred_port(card, PA_DIRECTION_OUTPUT, NULL);
        return;
    }

    sink = pa_idxset_first(card->sinks, NULL);

    /* We know the user wanted to activate this sink. The user might not have
     * wanted to activate the port that was selected by default, but if that's
     * the case, the user will change the port manually, and we'll update the
     * port preference at that time. If no port change occurs, we can assume
     * that the user likes the port that is now active. */
    pa_card_set_preferred_port(card, PA_DIRECTION_OUTPUT, sink->active_port);
}

static pa_hook_result_t card_profile_changed_callback(pa_core *core, pa_card *card, struct userdata *u) {
    struct card_info *info;
    pa_card_profile *old_profile;
    pa_card_profile *new_profile;

    info = pa_hashmap_get(u->card_infos, card);
    old_profile = info->active_profile;
    new_profile = card->active_profile;
    info->active_profile = new_profile;

    /* This profile change wasn't initiated by the user, so it doesn't signal
     * a change in the user's port preferences. */
    if (!card->save_profile)
        return PA_HOOK_OK;

    update_preferred_input_port(card, old_profile, new_profile);
    update_preferred_output_port(card, old_profile, new_profile);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_port_changed_callback(pa_core *core, pa_source *source, void *userdata) {
    if (!source->save_port)
        return PA_HOOK_OK;

    pa_card_set_preferred_port(source->card, PA_DIRECTION_INPUT, source->active_port);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_port_changed_callback(pa_core *core, pa_sink *sink, void *userdata) {
    if (!sink->save_port)
        return PA_HOOK_OK;

    pa_card_set_preferred_port(sink->card, PA_DIRECTION_OUTPUT, sink->active_port);

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_card *card;
    uint32_t idx;

    pa_assert(m);

    u = m->userdata = pa_xnew0(struct userdata, 1);
    u->card_infos = pa_hashmap_new(NULL, NULL);

    PA_IDXSET_FOREACH(card, m->core->cards, idx)
        card_info_new(u, card);

    /* Make sure we are after module-device-restore, so we can overwrite that suggestion if necessary */
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_NEW],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) sink_new_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_NEW],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) source_new_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_PORT_AVAILABLE_CHANGED],
                           PA_HOOK_LATE, (pa_hook_cb_t) port_available_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_CARD_PROFILE_AVAILABLE_CHANGED],
                           PA_HOOK_LATE, (pa_hook_cb_t) card_profile_available_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_CARD_PUT],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) card_put_hook_callback, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_CARD_UNLINK],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) card_unlink_hook_callback, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_CARD_PROFILE_CHANGED],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) card_profile_changed_callback, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_PORT_CHANGED],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) source_port_changed_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_PORT_CHANGED],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) sink_port_changed_callback, NULL);

    handle_all_unavailable(m->core);

    return 0;
}

void pa__done(pa_module *module) {
    struct userdata *u;
    struct card_info *info;

    pa_assert(module);

    if (!(u = module->userdata))
        return;

    while ((info = pa_hashmap_last(u->card_infos)))
        card_info_free(info);

    pa_hashmap_free(u->card_infos);

    pa_xfree(u);
}
