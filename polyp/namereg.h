#ifndef foonamereghfoo
#define foonamereghfoo

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

#include "core.h"

enum pa_namereg_type {
    PA_NAMEREG_SINK,
    PA_NAMEREG_SOURCE,
    PA_NAMEREG_SAMPLE
};

void pa_namereg_free(struct pa_core *c);

const char *pa_namereg_register(struct pa_core *c, const char *name, enum pa_namereg_type type, void *data, int fail);
void pa_namereg_unregister(struct pa_core *c, const char *name);
void* pa_namereg_get(struct pa_core *c, const char *name, enum pa_namereg_type type, int autoload);
void pa_namereg_set_default(struct pa_core*c, const char *name, enum pa_namereg_type type);


const char *pa_namereg_get_default_sink_name(struct pa_core *c);
const char *pa_namereg_get_default_source_name(struct pa_core *c);


#endif
