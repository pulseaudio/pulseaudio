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
    void (*callback)(struct pa_mainloop_api*m, void *userdata);
    void *userdata;
};

static void once_callback(struct pa_mainloop_api *m, struct pa_defer_event *e, void *userdata) {
    struct once_info *i = userdata;
    assert(m && i && i->callback);

    i->callback(m, i->userdata);

    assert(m->defer_free);
    m->defer_free(e);
}

static void free_callback(struct pa_mainloop_api *m, struct pa_defer_event *e, void *userdata) {
    struct once_info *i = userdata;
    assert(m && i);
    pa_xfree(i);
}

void pa_mainloop_api_once(struct pa_mainloop_api* m, void (*callback)(struct pa_mainloop_api *m, void *userdata), void *userdata) {
    struct once_info *i;
    struct pa_defer_event *e;
    assert(m && callback);

    i = pa_xmalloc(sizeof(struct once_info));
    i->callback = callback;
    i->userdata = userdata;

    assert(m->defer_new);
    e = m->defer_new(m, once_callback, i);
    assert(e);
    m->defer_set_destroy(e, free_callback);
}

