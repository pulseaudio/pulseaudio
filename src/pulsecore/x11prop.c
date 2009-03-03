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

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "x11prop.h"

void pa_x11_set_prop(Display *d, const char *name, const char *data) {
    Atom a = XInternAtom(d, name, False);
    XChangeProperty(d, RootWindow(d, 0), a, XA_STRING, 8, PropModeReplace, (const unsigned char*) data, (int) (strlen(data)+1));
}

void pa_x11_del_prop(Display *d, const char *name) {
    Atom a = XInternAtom(d, name, False);
    XDeleteProperty(d, RootWindow(d, 0), a);
}

char* pa_x11_get_prop(Display *d, const char *name, char *p, size_t l) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long nbytes_after;
    unsigned char *prop = NULL;
    char *ret = NULL;

    Atom a = XInternAtom(d, name, False);
    if (XGetWindowProperty(d, RootWindow(d, 0), a, 0, (long) ((l+2)/4), False, XA_STRING, &actual_type, &actual_format, &nitems, &nbytes_after, &prop) != Success)
        goto finish;

    if (actual_type != XA_STRING)
        goto finish;

    memcpy(p, prop, nitems);
    p[nitems] = 0;

    ret = p;

finish:

    if (prop)
        XFree(prop);

    return ret;
}
