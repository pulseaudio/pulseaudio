/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "client.h"

pa_client *pa_client_new(pa_core *core, const char *driver, const char *name) {
    pa_client *c;

    pa_core_assert_ref(core);

    c = pa_xnew(pa_client, 1);
    c->name = pa_xstrdup(name);
    c->driver = pa_xstrdup(driver);
    c->owner = NULL;
    c->core = core;

    c->kill = NULL;
    c->userdata = NULL;

    pa_assert_se(pa_idxset_put(core->clients, c, &c->index) >= 0);

    pa_log_info("Created %u \"%s\"", c->index, c->name);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_NEW, c->index);

    pa_core_check_quit(core);

    return c;
}

void pa_client_free(pa_client *c) {
    pa_assert(c);
    pa_assert(c->core);

    pa_idxset_remove_by_data(c->core->clients, c, NULL);

    pa_core_check_quit(c->core);

    pa_log_info("Freed %u \"%s\"", c->index, c->name);
    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_REMOVE, c->index);
    pa_xfree(c->name);
    pa_xfree(c->driver);
    pa_xfree(c);
}

void pa_client_kill(pa_client *c) {
    pa_assert(c);

    if (!c->kill) {
        pa_log_warn("kill() operation not implemented for client %u", c->index);
        return;
    }

    c->kill(c);
}

void pa_client_set_name(pa_client *c, const char *name) {
    pa_assert(c);

    pa_log_info("Client %u changed name from \"%s\" to \"%s\"", c->index, c->name, name);

    pa_xfree(c->name);
    c->name = pa_xstrdup(name);

    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_CHANGE, c->index);
}
