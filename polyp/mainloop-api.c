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

#include <assert.h>
#include <stdlib.h>

#include "mainloop-api.h"
#include "xmalloc.h"

struct once_info {
    void (*callback)(void *userdata);
    void *userdata;
};

static void once_callback(struct pa_mainloop_api *api, void *id, void *userdata) {
    struct once_info *i = userdata;
    assert(api && i && i->callback);
    i->callback(i->userdata);
    assert(api->cancel_fixed);
    api->cancel_fixed(api, id);
    pa_xfree(i);
}

void pa_mainloop_api_once(struct pa_mainloop_api* api, void (*callback)(void *userdata), void *userdata) {
    struct once_info *i;
    void *id;
    assert(api && callback);

    i = pa_xmalloc(sizeof(struct once_info));
    i->callback = callback;
    i->userdata = userdata;

    assert(api->source_fixed);
    id = api->source_fixed(api, once_callback, i);
    assert(id);

    /* Note: if the mainloop is destroyed before once_callback() was called, some memory is leaked. */
}

