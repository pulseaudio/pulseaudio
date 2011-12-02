#ifndef foopulsedeviceporthfoo
#define foopulsedeviceporthfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB
  Copyright 2011 David Henningsson, Canonical Ltd.

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

typedef struct pa_device_port pa_device_port;

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <inttypes.h>

#include <pulse/def.h>
#include <pulsecore/object.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/core.h>

struct pa_device_port {
    pa_object parent; /* Needed for reference counting */
    pa_core *core;

    char *name;
    char *description;

    unsigned priority;
    pa_port_available_t available;         /* PA_PORT_AVAILABLE_UNKNOWN, PA_PORT_AVAILABLE_NO or PA_PORT_AVAILABLE_YES */

    pa_proplist *proplist;
    pa_hashmap *profiles; /* Can be NULL. Does not own the profiles */
    pa_bool_t is_input:1;
    pa_bool_t is_output:1;

    /* .. followed by some implementation specific data */
};

PA_DECLARE_PUBLIC_CLASS(pa_device_port);
#define PA_DEVICE_PORT(s) (pa_device_port_cast(s))

#define PA_DEVICE_PORT_DATA(d) ((void*) ((uint8_t*) d + PA_ALIGN(sizeof(pa_device_port))))

pa_device_port *pa_device_port_new(pa_core *c, const char *name, const char *description, size_t extra);

void pa_device_port_hashmap_free(pa_hashmap *h);

/* The port's available status has changed */
void pa_device_port_set_available(pa_device_port *p, pa_port_available_t available);

#endif
