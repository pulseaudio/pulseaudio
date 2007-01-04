/* $Id$ */

/***
  This file is part of PulseAudio.

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
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>

#include "client.h"

pa_client *pa_client_new(pa_core *core, const char *driver, const char *name) {
    pa_client *c;
    int r;
    assert(core);

    c = pa_xmalloc(sizeof(pa_client));
    c->name = pa_xstrdup(name);
    c->driver = pa_xstrdup(driver);
    c->owner = NULL;
    c->core = core;

    c->kill = NULL;
    c->userdata = NULL;

    r = pa_idxset_put(core->clients, c, &c->index);
    assert(c->index != PA_IDXSET_INVALID && r >= 0);

    pa_log_info("created %u \"%s\"", c->index, c->name);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_NEW, c->index);

    pa_core_check_quit(core);

    return c;
}

void pa_client_free(pa_client *c) {
    assert(c && c->core);

    pa_idxset_remove_by_data(c->core->clients, c, NULL);

    pa_core_check_quit(c->core);

    pa_log_info("freed %u \"%s\"", c->index, c->name);
    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_REMOVE, c->index);
    pa_xfree(c->name);
    pa_xfree(c->driver);
    pa_xfree(c);
}

void pa_client_kill(pa_client *c) {
    assert(c);
    if (!c->kill) {
        pa_log_warn("kill() operation not implemented for client %u", c->index);
        return;
    }

    c->kill(c);
}

void pa_client_set_name(pa_client *c, const char *name) {
    assert(c);

    pa_log_info("client %u changed name from \"%s\" to \"%s\"", c->index, c->name, name);

    pa_xfree(c->name);
    c->name = pa_xstrdup(name);

    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_CHANGE, c->index);
}
