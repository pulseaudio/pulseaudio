#ifndef foomodulehfoo
#define foomodulehfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>
#include <ltdl.h>

#include "core.h"
#include "modinfo.h"

struct pa_module {
    struct pa_core *core;
    char *name, *argument;
    uint32_t index;

    lt_dlhandle dl;
    
    int (*init)(struct pa_core *c, struct pa_module*m);
    void (*done)(struct pa_core *c, struct pa_module*m);

    void *userdata;

    int n_used;
    int auto_unload;
    time_t last_used_time;

    int unload_requested;
};

struct pa_module* pa_module_load(struct pa_core *c, const char *name, const char*argument);
/* void pa_module_unload(struct pa_core *c, struct pa_module *m); */
/* void pa_module_unload_by_index(struct pa_core *c, uint32_t index); */

void pa_module_unload_all(struct pa_core *c);
void pa_module_unload_unused(struct pa_core *c);

void pa_module_unload_request(struct pa_module *m);

void pa_module_set_used(struct pa_module*m, int used);

/* prototypes for the module's entry points */
int pa__init(struct pa_core *c, struct pa_module*m);
void pa__done(struct pa_core *c, struct pa_module*m);

#define PA_MODULE_AUTHOR(s) const char *pa__get_author(void) { return s; }
#define PA_MODULE_DESCRIPTION(s) const char *pa__get_description(void) { return s; }
#define PA_MODULE_USAGE(s) const char *pa__get_usage(void) { return s; }
#define PA_MODULE_VERSION(s) const char *pa__get_version(void) { return s; }

struct pa_modinfo *pa_module_get_info(struct pa_module *m);

#endif
