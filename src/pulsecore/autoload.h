#ifndef fooautoloadhfoo
#define fooautoloadhfoo

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

#include <pulsecore/namereg.h>

/* Using the autoloading facility, modules by be loaded on-demand and
 * synchronously. The user may register a "ghost sink" or "ghost
 * source". Whenever this sink/source is requested but not available a
 * specified module is loaded. */

/* An autoload entry, or "ghost" sink/source */
typedef struct pa_autoload_entry {
    pa_core *core;
    uint32_t index;
    char *name;
    pa_namereg_type_t type; /* Type of the autoload entry */
    int in_action; /* The module is currently being loaded */
    char *module, *argument;
} pa_autoload_entry;

/* Add a new autoload entry of the given time, with the speicified
 * sink/source name, module name and argument. Return the entry's
 * index in *index */
int pa_autoload_add(pa_core *c, const char*name, pa_namereg_type_t type, const char*module, const char *argument, uint32_t *idx);

/* Free all autoload entries */
void pa_autoload_free(pa_core *c);
int pa_autoload_remove_by_name(pa_core *c, const char*name, pa_namereg_type_t type);
int pa_autoload_remove_by_index(pa_core *c, uint32_t idx);

/* Request an autoload entry by its name, effectively causing a module to be loaded */
void pa_autoload_request(pa_core *c, const char *name, pa_namereg_type_t type);

const pa_autoload_entry* pa_autoload_get_by_name(pa_core *c, const char*name, pa_namereg_type_t type);
const pa_autoload_entry* pa_autoload_get_by_index(pa_core *c, uint32_t idx);

#endif
