#ifndef foonodehfoo
#define foonodehfoo

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

typedef struct pa_node_new_data pa_node_new_data;
typedef struct pa_node pa_node;

#include <pulsecore/core.h>

/* The node type determines what the owner pointer of pa_node points to. */
typedef enum {
    PA_NODE_TYPE_PORT,          /* owner: pa_port */
    PA_NODE_TYPE_SINK,          /* owner: pa_sink */
    PA_NODE_TYPE_SOURCE,        /* owner: pa_source */
    PA_NODE_TYPE_SINK_INPUT,    /* owner: pa_sink_input */
    PA_NODE_TYPE_SOURCE_OUTPUT  /* owner: pa_source_output */
} pa_node_type_t;

typedef enum {
    PA_NODE_STATE_INIT,
    PA_NODE_STATE_LINKED,
    PA_NODE_STATE_UNLINKED
} pa_node_state_t;

struct pa_node_new_data {
    /* Node names are generated automatically as much as possible, but
     * sometimes the available information for automatic generation isn't
     * sufficient, in which case the generated node names would be just "input"
     * or "output". In such cases the fallback name prefix, if set, is used to
     * generate slightly more informative names, such as "jack-output" for JACK
     * output nodes (in this example the fallback prefix would be "jack"). */
    char *fallback_name_prefix;

    char *description;

    pa_node_type_t type;
    pa_direction_t direction;
};

struct pa_node {
    pa_core *core;

    uint32_t index;
    char *name;
    char *description;

    pa_node_type_t type;
    pa_direction_t direction;

    pa_node_state_t state;

    void *owner;
};

pa_node_new_data *pa_node_new_data_init(pa_node_new_data *data);
void pa_node_new_data_set_fallback_name_prefix(pa_node_new_data *data, const char *prefix);
void pa_node_new_data_set_description(pa_node_new_data *data, const char *description);
void pa_node_new_data_set_type(pa_node_new_data *data, pa_node_type_t type);
void pa_node_new_data_set_direction(pa_node_new_data *data, pa_direction_t direction);
void pa_node_new_data_done(pa_node_new_data *data);

pa_node *pa_node_new(pa_core *core, pa_node_new_data *data);
void pa_node_free(pa_node *node);

void pa_node_put(pa_node *node);
void pa_node_unlink(pa_node *node);

#endif
