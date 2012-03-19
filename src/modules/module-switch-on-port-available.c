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
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/device-port.h>
#include <pulsecore/hashmap.h>

#include "module-switch-on-port-available-symdef.h"

struct userdata {
     pa_hook_slot *callback_slot;
};

static pa_device_port* find_best_port(pa_hashmap *ports) {
    void *state;
    pa_device_port* port, *result = NULL;

    PA_HASHMAP_FOREACH(port, ports, state) {
        if (result == NULL ||
            result->available == PA_PORT_AVAILABLE_NO ||
            (port->available != PA_PORT_AVAILABLE_NO && port->priority > result->priority)) {
            result = port;
        }
    }

    return result;
}

static pa_bool_t try_to_switch_profile(pa_card *card, pa_device_port *port) {
    pa_card_profile *best_profile = NULL, *profile;
    void *state;

    pa_log_debug("Finding best profile");

    if (port->profiles)
        PA_HASHMAP_FOREACH(profile, port->profiles, state) {
            if (best_profile && best_profile->priority >= profile->priority)
                continue;

            if (!card->active_profile) {
                best_profile = profile;
                continue;
            }

            /* We make a best effort to keep other direction unchanged */
            if (!port->is_input) {
                if (card->active_profile->n_sources != profile->n_sources)
                    continue;

                if (card->active_profile->max_source_channels != profile->max_source_channels)
                    continue;
            }

            if (!port->is_output) {
                if (card->active_profile->n_sinks != profile->n_sinks)
                    continue;

                if (card->active_profile->max_sink_channels != profile->max_sink_channels)
                    continue;
            }

            if (port->is_output) {
                /* Try not to switch to HDMI sinks from analog when HDMI is becoming available */
                uint32_t state2;
                pa_sink *sink;
                pa_bool_t found_active_port = FALSE;
                PA_IDXSET_FOREACH(sink, card->sinks, state2) {
                    if (!sink->active_port)
                        continue;
                    if (sink->active_port->available != PA_PORT_AVAILABLE_NO)
                        found_active_port = TRUE;
                }
                if (found_active_port)
                    continue;
            }

            best_profile = profile;
        }

    if (!best_profile) {
        pa_log_debug("No suitable profile found");
        return FALSE;
    }

    if (pa_card_set_profile(card, best_profile->name, FALSE) != 0) {
        pa_log_debug("Could not set profile %s", best_profile->name);
        return FALSE;
    }

    return TRUE;
}

static void find_sink_and_source(pa_card *card, pa_device_port *port, pa_sink **si, pa_source **so)
{
    pa_sink *sink = NULL;
    pa_source *source = NULL;
    uint32_t state;

    if (port->is_output)
        PA_IDXSET_FOREACH(sink, card->sinks, state)
            if (sink->ports && port == pa_hashmap_get(sink->ports, port->name))
                break;

    if (port->is_input)
        PA_IDXSET_FOREACH(source, card->sources, state)
            if (source->ports && port == pa_hashmap_get(source->ports, port->name))
                break;

    *si = sink;
    *so = source;
}

static pa_hook_result_t port_available_hook_callback(pa_core *c, pa_device_port *port, void* userdata) {
    uint32_t state;
    pa_card* card;
    pa_sink *sink;
    pa_source *source;
    pa_bool_t is_active_profile, is_active_port;

    if (port->available == PA_PORT_AVAILABLE_UNKNOWN)
        return PA_HOOK_OK;

    pa_log_debug("finding port %s", port->name);

    PA_IDXSET_FOREACH(card, c->cards, state)
        if (card->ports && port == pa_hashmap_get(card->ports, port->name))
            break;

    if (!card) {
        pa_log_warn("Did not find port %s in array of cards", port->name);
        return PA_HOOK_OK;
    }

    find_sink_and_source(card, port, &sink, &source);

    is_active_profile = port->profiles && card->active_profile &&
        card->active_profile == pa_hashmap_get(port->profiles, card->active_profile->name);
    is_active_port = (sink && sink->active_port == port) || (source && source->active_port == port);

    if (port->available == PA_PORT_AVAILABLE_NO && !is_active_port)
        return PA_HOOK_OK;

    if (port->available == PA_PORT_AVAILABLE_YES) {
        if (is_active_port)
            return PA_HOOK_OK;

        if (!is_active_profile) {
            if (!try_to_switch_profile(card, port))
                return PA_HOOK_OK;

            pa_assert(card->active_profile == pa_hashmap_get(port->profiles, card->active_profile->name));

            /* Now that profile has changed, our sink and source pointers must be updated */
            find_sink_and_source(card, port, &sink, &source);
        }

        if (source)
            pa_source_set_port(source, port->name, FALSE);
        if (sink)
            pa_sink_set_port(sink, port->name, FALSE);
    }

    if (port->available == PA_PORT_AVAILABLE_NO) {
        if (sink) {
            pa_device_port *p2 = find_best_port(sink->ports);

            if (p2 && p2->available != PA_PORT_AVAILABLE_NO)
                pa_sink_set_port(sink, p2->name, FALSE);
            else {
                /* Maybe try to switch to another profile? */
            }
        }

        if (source) {
            pa_device_port *p2 = find_best_port(source->ports);

            if (p2 && p2->available != PA_PORT_AVAILABLE_NO)
                pa_source_set_port(source, p2->name, FALSE);
            else {
                /* Maybe try to switch to another profile? */
            }
        }
    }

    return PA_HOOK_OK;
}

static void handle_all_unavailable(pa_core *core) {
    pa_card *card;
    uint32_t state;

    PA_IDXSET_FOREACH(card, core->cards, state) {
        pa_device_port *port;
        void *state2;

        if (!card->ports)
            continue;

        PA_HASHMAP_FOREACH(port, card->ports, state2) {
            if (port->available == PA_PORT_AVAILABLE_NO)
                port_available_hook_callback(core, port, NULL);
        }
    }
}

int pa__init(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    m->userdata = u = pa_xnew(struct userdata, 1);

    u->callback_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_PORT_AVAILABLE_CHANGED],
                                       PA_HOOK_LATE, (pa_hook_cb_t) port_available_hook_callback, u);

    handle_all_unavailable(m->core);

    return 0;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->callback_slot)
        pa_hook_slot_free(u->callback_slot);

    pa_xfree(u);
}
