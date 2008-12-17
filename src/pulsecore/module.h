#ifndef foomodulehfoo
#define foomodulehfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <inttypes.h>
#include <ltdl.h>

typedef struct pa_module pa_module;

#include <pulsecore/core.h>
#include <pulsecore/modinfo.h>

struct pa_module {
    pa_core *core;
    char *name, *argument;
    uint32_t index;

    lt_dlhandle dl;

    int (*init)(pa_module*m);
    void (*done)(pa_module*m);

    void *userdata;

    int n_used;

    pa_bool_t auto_unload:1;
    pa_bool_t load_once:1;
    pa_bool_t unload_requested:1;

    time_t last_used_time;
};

pa_module* pa_module_load(pa_core *c, const char *name, const char*argument);

void pa_module_unload(pa_core *c, pa_module *m, pa_bool_t force);
void pa_module_unload_by_index(pa_core *c, uint32_t idx, pa_bool_t force);

void pa_module_unload_request(pa_module *m, pa_bool_t force);
void pa_module_unload_request_by_index(pa_core *c, uint32_t idx, pa_bool_t force);

void pa_module_unload_all(pa_core *c);
void pa_module_unload_unused(pa_core *c);

void pa_module_set_used(pa_module*m, int used);

#define PA_MODULE_AUTHOR(s)                                     \
    const char *pa__get_author(void) { return s; }              \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

#define PA_MODULE_DESCRIPTION(s)                                \
    const char *pa__get_description(void) { return s; }         \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

#define PA_MODULE_USAGE(s)                                      \
    const char *pa__get_usage(void) { return s; }               \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

#define PA_MODULE_VERSION(s)                                    \
    const char * pa__get_version(void) { return s; }            \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

#define PA_MODULE_LOAD_ONCE(b)                                  \
    pa_bool_t pa__load_once(void) { return b; }                 \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

pa_modinfo *pa_module_get_info(pa_module *m);

#endif
