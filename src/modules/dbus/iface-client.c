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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>
#include <pulsecore/protocol-dbus.h>

#include "iface-client.h"

#define OBJECT_NAME "client"

struct pa_dbusiface_client {
    pa_client *client;
    char *path;
};

pa_dbusiface_client *pa_dbusiface_client_new(pa_dbusiface_core *core, pa_client *client) {
    pa_dbusiface_client *c;

    pa_assert(core);
    pa_assert(client);

    c = pa_xnew(pa_dbusiface_client, 1);
    c->client = client;
    c->path = pa_sprintf_malloc("%s/%s%u", PA_DBUS_CORE_OBJECT_PATH, OBJECT_NAME, client->index);

    return c;
}

void pa_dbusiface_client_free(pa_dbusiface_client *c) {
    pa_assert(c);

    pa_xfree(c->path);
    pa_xfree(c);
}

const char *pa_dbusiface_client_get_path(pa_dbusiface_client *c) {
    pa_assert(c);

    return c->path;
}
