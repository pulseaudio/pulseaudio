/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <string.h>

#include "x11prop.h"

#include <pulsecore/macro.h>

#include <xcb/xproto.h>
#include <xcb/xcb_atom.h>

#define PA_XCB_FORMAT 8

void pa_x11_set_prop(xcb_connection_t *xcb, const char *name, const char *data) {
    xcb_screen_t *screen;
    const xcb_setup_t *s;
    xcb_atom_t a;

    pa_assert(xcb);
    pa_assert(name);
    pa_assert(data);

    if ((s = xcb_get_setup(xcb))) {
        a = xcb_atom_get(xcb, name);
        screen = xcb_setup_roots_iterator(s).data;
        xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, screen->root, a, STRING, PA_XCB_FORMAT, (int) strlen(data), (const void*) data);
    }
}

void pa_x11_del_prop(xcb_connection_t *xcb, const char *name) {
    xcb_screen_t *screen;
    const xcb_setup_t *s;
    xcb_atom_t a;

    pa_assert(xcb);
    pa_assert(name);

    if ((s = xcb_get_setup(xcb))) {
        a = xcb_atom_get(xcb, name);
        screen = xcb_setup_roots_iterator(s).data;
        xcb_delete_property(xcb, screen->root, a);
    }
}

char* pa_x11_get_prop(xcb_connection_t *xcb, const char *name, char *p, size_t l) {
    char *ret = NULL;
    int len;
    xcb_get_property_cookie_t req;
    xcb_get_property_reply_t* prop = NULL;
    xcb_screen_t *screen;
    const xcb_setup_t *s;
    xcb_atom_t a;

    pa_assert(xcb);
    pa_assert(name);
    pa_assert(p);

    if ((s = xcb_get_setup(xcb))) {
        a = xcb_atom_get(xcb, name);
        screen = xcb_setup_roots_iterator(s).data;

        req = xcb_get_property(xcb, 0, screen->root, a, STRING, 0, (uint32_t)(l-1));
        prop = xcb_get_property_reply(xcb, req, NULL);

        if (!prop)
            goto finish;

        if (PA_XCB_FORMAT != prop->format)
            goto finish;

        len = xcb_get_property_value_length(prop);
        if (len < 1 || len >= (int)l)
            goto finish;

        memcpy(p, xcb_get_property_value(prop), len);
        p[len] = 0;

        ret = p;
    }

finish:

    if (prop)
        free(prop);

    return ret;
}
