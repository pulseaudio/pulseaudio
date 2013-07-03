/***
  This file is part of PulseAudio.

  Copyright (c) 2012 Intel Corporation
  Janos Kovacs <jankovac503@gmail.com>

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

#include <pulsecore/core-util.h>
#include <pulsecore/namereg.h>
#include <pulsecore/strbuf.h>

#include "node.h"

pa_node_new_data *pa_node_new_data_init(pa_node_new_data *data) {
    pa_assert(data);

    pa_zero(*data);
    data->direction = PA_DIRECTION_OUTPUT;

    return data;
}

void pa_node_new_data_set_fallback_name_prefix(pa_node_new_data *data, const char* prefix) {
    pa_assert(data);

    pa_xfree(data->fallback_name_prefix);
    data->fallback_name_prefix = pa_xstrdup(prefix);
}

void pa_node_new_data_set_description(pa_node_new_data *data, const char *description) {
    pa_assert(data);

    pa_xfree(data->description);
    data->description = pa_xstrdup(description);
}

void pa_node_new_data_set_type(pa_node_new_data *data, pa_node_type_t type) {
    pa_assert(data);

    data->type = type;
}

void pa_node_new_data_set_direction(pa_node_new_data *data, pa_direction_t direction) {
    pa_assert(data);

    data->direction = direction;
}

void pa_node_new_data_done(pa_node_new_data *data) {
    pa_assert(data);

    pa_xfree(data->description);
    pa_xfree(data->fallback_name_prefix);
}

pa_node *pa_node_new(pa_core *core, pa_node_new_data *data) {
    bool use_fallback_name_prefix;
    pa_strbuf *name_buf;
    char *name = NULL;
    pa_node *n = NULL;
    const char *registered_name = NULL;

    pa_assert(core);
    pa_assert(data);
    pa_assert(data->description);

    use_fallback_name_prefix = !!data->fallback_name_prefix;

    name_buf = pa_strbuf_new();

    /* Automatic name generation code will appear here... */

    if (use_fallback_name_prefix)
        pa_strbuf_printf(name_buf, "%s-", data->fallback_name_prefix);

    pa_strbuf_puts(name_buf, data->direction == PA_DIRECTION_OUTPUT ? "output" : "input");

    name = pa_strbuf_tostring_free(name_buf);

    n = pa_xnew0(pa_node, 1);
    n->core = core;
    n->state = PA_NODE_STATE_INIT;

    if (!(registered_name = pa_namereg_register(core, name, PA_NAMEREG_NODE, n, false))) {
        pa_log("Failed to register name %s.", name);
        goto fail;
    }

    pa_xfree(name);

    n->name = pa_xstrdup(registered_name);
    n->description = pa_xstrdup(data->description);
    n->type = data->type;
    n->direction = data->direction;

    return n;

fail:
    pa_xfree(name);
    pa_node_free(n);

    return NULL;
}

void pa_node_free(pa_node *node) {
    pa_assert(node);

    if (node->state == PA_NODE_STATE_LINKED)
        pa_node_unlink(node);

    pa_xfree(node->description);

    if (node->name) {
        pa_namereg_unregister(node->core, node->name);
        pa_xfree(node->name);
    }

    pa_xfree(node);
}

void pa_node_put(pa_node *node) {
    pa_assert(node);
    pa_assert(node->state == PA_NODE_STATE_INIT);
    pa_assert(node->owner);

    pa_assert_se(pa_idxset_put(node->core->nodes, node, &node->index) >= 0);

    node->state = PA_NODE_STATE_LINKED;

    pa_log_debug("Created node %s.", node->name);
}

void pa_node_unlink(pa_node *node) {
    pa_assert(node);
    pa_assert(node->state != PA_NODE_STATE_INIT);

    if (node->state == PA_NODE_STATE_UNLINKED)
        return;

    pa_log_debug("Unlinking node %s.", node->name);

    pa_assert_se(pa_idxset_remove_by_index(node->core->nodes, node->index));

    node->state = PA_NODE_STATE_UNLINKED;
}
