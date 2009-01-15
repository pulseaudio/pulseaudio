#ifndef foopulsecardhfoo
#define foopulsecardhfoo

/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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

typedef struct pa_card pa_card;

#include <pulse/proplist.h>
#include <pulsecore/core.h>
#include <pulsecore/module.h>
#include <pulsecore/idxset.h>

typedef struct pa_card_config {
    char *name;

    pa_bool_t optical_sink:1;
    pa_bool_t optical_source:1;

    unsigned n_sinks;
    unsigned n_sources;

    unsigned max_sink_channels;
    unsigned max_source_channels;
} pa_card_config;

struct pa_card {
    uint32_t index;
    pa_core *core;

    char *name;

    pa_proplist *proplist;
    pa_module *module;
    char *driver;

    pa_idxset *sinks;
    pa_idxset *sources;

    pa_hashmap *configs;
    pa_card_config *active_config;

    void *userdata;

    int (*set_config)(pa_card *c, pa_card_config *config);
};

typedef struct pa_card_new_data {
    char *name;

    pa_proplist *proplist;
    const char *driver;
    pa_module *module;

    pa_hashmap *configs;
    pa_card_config *active_config;

    pa_bool_t namereg_fail:1;
} pa_card_new_data;

pa_card_config *pa_card_config_new(const char *name);
void pa_card_config_free(pa_card_config *c);

pa_card_new_data *pa_card_new_data_init(pa_card_new_data *data);
void pa_card_new_data_set_name(pa_card_new_data *data, const char *name);
void pa_card_new_data_done(pa_card_new_data *data);

pa_card *pa_card_new(pa_core *c, pa_card_new_data *data);
void pa_card_free(pa_card *c);

int pa_card_set_config(pa_card *c, const char *name);

#endif
