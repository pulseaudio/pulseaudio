#ifndef foopropshfoo
#define foopropshfoo

/* $Id$ */

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

#include <pulsecore/core.h>
#include <pulsecore/strbuf.h>

/* The property subsystem is to be used to share data between
 * modules. Consider them to be kind of "global" variables for a
 * core. Why not use the hashmap functions directly? The hashmap
 * functions copy neither the key nor value, while this property
 * system copies the key. Users of this system have to think about
 * reference counting themselves. */

/* Return a pointer to the value of the specified property. */
void* pa_property_get(pa_core *c, const char *name);

/* Set the property 'name' to 'data'. This function fails in case a
 * property by this name already exists. The property data is not
 * copied or reference counted. This is the caller's job. */
int pa_property_set(pa_core *c, const char *name, void *data);

/* Remove the specified property. Return non-zero on failure */
int pa_property_remove(pa_core *c, const char *name);

/* A combination of pa_property_remove() and pa_property_set() */
int pa_property_replace(pa_core *c, const char *name, void *data);

/* Free all memory used by the property system */
void pa_property_cleanup(pa_core *c);

/* Initialize the properties subsystem */
void pa_property_init(pa_core *c);

/* Dump the current set of properties */
void pa_property_dump(pa_core *c, pa_strbuf *s);

#endif
