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


#include "device-port.h"

PA_DEFINE_PUBLIC_CLASS(pa_device_port, pa_object);

static void device_port_free(pa_object *o) {
    pa_device_port *p = PA_DEVICE_PORT(o);

    pa_assert(p);
    pa_assert(pa_device_port_refcnt(p) == 0);

    pa_xfree(p->name);
    pa_xfree(p->description);
    pa_xfree(p);
}


pa_device_port *pa_device_port_new(const char *name, const char *description, size_t extra) {
    pa_device_port *p;

    pa_assert(name);

    p = PA_DEVICE_PORT(pa_object_new_internal(PA_ALIGN(sizeof(pa_device_port)) + extra, pa_device_port_type_id, pa_device_port_check_type));
    p->parent.free = device_port_free;

    p->name = pa_xstrdup(name);
    p->description = pa_xstrdup(description);
    p->priority = 0;
    p->available = PA_PORT_AVAILABLE_UNKNOWN;

    return p;
}

void pa_device_port_hashmap_free(pa_hashmap *h) {
    pa_device_port *p;

    pa_assert(h);

    while ((p = pa_hashmap_steal_first(h)))
        pa_device_port_unref(p);

    pa_hashmap_free(h, NULL, NULL);
}
