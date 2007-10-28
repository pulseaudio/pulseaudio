/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/props.h>
#include <pulsecore/macro.h>
#include <pulsecore/log.h>
#include <pulsecore/refcnt.h>

#include "authkey-prop.h"

struct authkey_data {
    PA_REFCNT_DECLARE;
    size_t length;
};

int pa_authkey_prop_get(pa_core *c, const char *name, void *data, size_t len) {
    struct authkey_data *a;

    pa_assert(c);
    pa_assert(name);
    pa_assert(data);
    pa_assert(len > 0);

    if (!(a = pa_property_get(c, name)))
        return -1;

    pa_assert(a->length == len);
    memcpy(data, (uint8_t*) a + PA_ALIGN(sizeof(struct authkey_data)), len);

    return 0;
}

int pa_authkey_prop_put(pa_core *c, const char *name, const void *data, size_t len) {
    struct authkey_data *a;

    pa_assert(c);
    pa_assert(name);

    if (pa_property_get(c, name))
        return -1;

    a = pa_xmalloc(PA_ALIGN(sizeof(struct authkey_data)) + len);
    PA_REFCNT_INIT(a);
    a->length = len;
    memcpy((uint8_t*) a + PA_ALIGN(sizeof(struct authkey_data)), data, len);

    pa_property_set(c, name, a);

    return 0;
}

void pa_authkey_prop_ref(pa_core *c, const char *name) {
    struct authkey_data *a;

    pa_assert(c);
    pa_assert(name);

    a = pa_property_get(c, name);
    pa_assert(a);
    pa_assert(PA_REFCNT_VALUE(a) >= 1);
    PA_REFCNT_INC(a);
}

void pa_authkey_prop_unref(pa_core *c, const char *name) {
    struct authkey_data *a;

    pa_assert(c);
    pa_assert(name);

    a = pa_property_get(c, name);
    pa_assert(a);
    pa_assert(PA_REFCNT_VALUE(a) >= 1);

    if (PA_REFCNT_DEC(a) <= 0) {
        pa_property_remove(c, name);
        pa_xfree(a);
    }
}


