/* $Id$ */

/***
  This file is part of polypaudio.

  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-13071
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <assert.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <polypcore/x11prop.h>
#include <polypcore/log.h>
#include <polypcore/xmalloc.h>
#include <polypcore/util.h>

#include "client-conf-x11.h"

int pa_client_conf_from_x11(pa_client_conf *c, const char *dname) {
    Display *d = NULL;
    int ret = -1;
    char t[1024];

    if (!dname && !getenv("DISPLAY"))
        goto finish;
    
    if (!(d = XOpenDisplay(dname))) {
        pa_log(__FILE__": XOpenDisplay() failed\n");
        goto finish;
    }

    if (pa_x11_get_prop(d, "POLYP_SERVER", t, sizeof(t))) {
        pa_xfree(c->default_server);
        c->default_server = pa_xstrdup(t);
    }

    if (pa_x11_get_prop(d, "POLYP_SINK", t, sizeof(t))) {
        pa_xfree(c->default_sink);
        c->default_sink = pa_xstrdup(t);
    }

    if (pa_x11_get_prop(d, "POLYP_SOURCE", t, sizeof(t))) {
        pa_xfree(c->default_source);
        c->default_source = pa_xstrdup(t);
    }

    if (pa_x11_get_prop(d, "POLYP_COOKIE", t, sizeof(t))) {
        uint8_t cookie[PA_NATIVE_COOKIE_LENGTH];

        if (pa_parsehex(t, cookie, sizeof(cookie)) != sizeof(cookie)) {
            pa_log(__FILE__": failed to parse cookie data\n");
            goto finish;
        }

        assert(sizeof(cookie) == sizeof(c->cookie));
        memcpy(c->cookie, cookie, sizeof(cookie));

        c->cookie_valid = 1;

        pa_xfree(c->cookie_file);
        c->cookie_file = NULL;
    }

    ret = 0;

finish:
    if (d)
        XCloseDisplay(d);

    return ret;
    
}
