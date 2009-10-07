#ifndef fooserverlookuphfoo
#define fooserverlookuphfoo

/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

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

/* This object implements the D-Bus object at path
 * /org/pulseaudio/server_lookup. Implemented interfaces
 * are org.pulseaudio.ServerLookup and org.freedesktop.DBus.Introspectable.
 *
 * See http://pulseaudio.org/wiki/DBusInterface for the ServerLookup interface
 * documentation.
 */

#include <pulsecore/core.h>

typedef struct pa_dbusobj_server_lookup pa_dbusobj_server_lookup;

pa_dbusobj_server_lookup *pa_dbusobj_server_lookup_new(pa_core *c);
void pa_dbusobj_server_lookup_free(pa_dbusobj_server_lookup *sl);

#endif
