/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <assert.h>

#include <polyp/xmalloc.h>

#include <polypcore/log.h>
#include <polypcore/props.h>

#include "howl-wrap.h"

#define HOWL_PROPERTY "howl"

struct pa_howl_wrapper {
    pa_core *core;
    int ref;

    pa_io_event *io_event;
    sw_discovery discovery;
};

static void howl_io_event(pa_mainloop_api*m, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata) {
    pa_howl_wrapper *w = userdata;
    assert(m && e && fd >= 0 && w && w->ref >= 1);

    if (f & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR))
        goto fail;

    if (sw_discovery_read_socket(w->discovery) != SW_OKAY)
        goto fail;

    return;

fail:
    pa_log_error(__FILE__": howl connection died.");
    w->core->mainloop->io_free(w->io_event);
    w->io_event = NULL;
}

static pa_howl_wrapper* howl_wrapper_new(pa_core *c) {
    pa_howl_wrapper *h;
    sw_discovery session;
    assert(c);

    if (sw_discovery_init(&session) != SW_OKAY) {
        pa_log_error(__FILE__": sw_discovery_init() failed.");
        return NULL;
    }

    h = pa_xmalloc(sizeof(pa_howl_wrapper));
    h->core = c;
    h->ref = 1;
    h->discovery = session;

    h->io_event = c->mainloop->io_new(c->mainloop, sw_discovery_socket(session), PA_IO_EVENT_INPUT, howl_io_event, h);

    return h;
}

static void howl_wrapper_free(pa_howl_wrapper *h) {
    assert(h);

    sw_discovery_fina(h->discovery);

    if (h->io_event)
        h->core->mainloop->io_free(h->io_event);

    pa_xfree(h);
}

pa_howl_wrapper* pa_howl_wrapper_get(pa_core *c) {
    pa_howl_wrapper *h;
    assert(c);
    
    if ((h = pa_property_get(c, HOWL_PROPERTY)))
        return pa_howl_wrapper_ref(h);

    return howl_wrapper_new(c);
}

pa_howl_wrapper* pa_howl_wrapper_ref(pa_howl_wrapper *h) {
    assert(h && h->ref >= 1);
    h->ref++;
    return h;
}

void pa_howl_wrapper_unref(pa_howl_wrapper *h) {
    assert(h && h->ref >= 1);
    if (!(--h->ref))
        howl_wrapper_free(h);
}

sw_discovery pa_howl_wrapper_get_discovery(pa_howl_wrapper *h) {
    assert(h && h->ref >= 1);

    return h->discovery;
}

