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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "xmalloc.h"
#include "subscribe.h"

struct pa_client *pa_client_new(struct pa_core *core, const char *protocol_name, char *name) {
    struct pa_client *c;
    int r;
    assert(core);

    c = pa_xmalloc(sizeof(struct pa_client));
    c->name = pa_xstrdup(name);
    c->owner = NULL;
    c->core = core;
    c->protocol_name = protocol_name;

    c->kill = NULL;
    c->userdata = NULL;

    r = pa_idxset_put(core->clients, c, &c->index);
    assert(c->index != PA_IDXSET_INVALID && r >= 0);

    fprintf(stderr, "client: created %u \"%s\"\n", c->index, c->name);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_NEW, c->index);
    
    return c;
}

void pa_client_free(struct pa_client *c) {
    assert(c && c->core);

    pa_idxset_remove_by_data(c->core->clients, c, NULL);
    fprintf(stderr, "client: freed %u \"%s\"\n", c->index, c->name);
    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CLIENT|PA_SUBSCRIPTION_EVENT_REMOVE, c->index);
    pa_xfree(c->name);
    pa_xfree(c);
}

void pa_client_kill(struct pa_client *c) {
    assert(c);
    if (!c->kill) {
        fprintf(stderr, "kill() operation not implemented for client %u\n", c->index);
        return;
    }

    c->kill(c);
}

void pa_client_rename(struct pa_client *c, const char *name) {
    assert(c);
    pa_xfree(c->name);
    c->name = pa_xstrdup(name);
}
