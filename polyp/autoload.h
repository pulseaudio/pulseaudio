#ifndef fooautoloadhfoo
#define fooautoloadhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include "namereg.h"

/* Using the autoloading facility, modules by be loaded on-demand and
 * synchronously. The user may register a "ghost sink" or "ghost
 * source". Whenever this sink/source is requested but not available a
 * specified module is loaded. */

/* An autoload entry, or "ghost" sink/source */
struct pa_autoload_entry {
    struct pa_core *core;
    uint32_t index;
    char *name;
    enum pa_namereg_type type; /* Type of the autoload entry */
    int in_action; /* Currently loaded */
    char *module, *argument;   
};

/* Add a new autoload entry of the given time, with the speicified
 * sink/source name, module name and argument. Return the entry's
 * index in *index */
int pa_autoload_add(struct pa_core *c, const char*name, enum pa_namereg_type type, const char*module, const char *argument, uint32_t *index);

/* Free all autoload entries */
void pa_autoload_free(struct pa_core *c);
int pa_autoload_remove_by_name(struct pa_core *c, const char*name, enum pa_namereg_type type);
int pa_autoload_remove_by_index(struct pa_core *c, uint32_t index);

/* Request an autoload entry by its name, effectively causing a module to be loaded */
void pa_autoload_request(struct pa_core *c, const char *name, enum pa_namereg_type type);

const struct pa_autoload_entry* pa_autoload_get_by_name(struct pa_core *c, const char*name, enum pa_namereg_type type);
const struct pa_autoload_entry* pa_autoload_get_by_index(struct pa_core *c, uint32_t index);

#endif
