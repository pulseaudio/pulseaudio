#ifndef foomodargshfoo
#define foomodargshfoo

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

#include <inttypes.h>
#include "sample.h"
#include "core.h"

typedef struct pa_modargs pa_modargs;

/* A generic parser for module arguments */

/* Parse the string args. The NULL-terminated array keys contains all valid arguments. */
pa_modargs *pa_modargs_new(const char *args, const char* const keys[]);
void pa_modargs_free(pa_modargs*ma);

/* Return the module argument for the specified name as a string. If
 * the argument was not specified, return def instead.*/
const char *pa_modargs_get_value(pa_modargs *ma, const char *key, const char *def);

/* Return a module argument as unsigned 32bit value in *value */
int pa_modargs_get_value_u32(pa_modargs *ma, const char *key, uint32_t *value);
int pa_modargs_get_value_s32(pa_modargs *ma, const char *key, int32_t *value);
int pa_modargs_get_value_boolean(pa_modargs *ma, const char *key, int *value);

/* Return sample spec data from the three arguments "rate", "format" and "channels" */
int pa_modargs_get_sample_spec(pa_modargs *ma, pa_sample_spec *ss);

#endif
