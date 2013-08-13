/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>

#include "bluez5-util.h"

struct pa_bluetooth_discovery {
    PA_REFCNT_DECLARE;

    pa_core *core;
};

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *c) {
    pa_bluetooth_discovery *y;

    if ((y = pa_shared_get(c, "bluetooth-discovery")))
        return pa_bluetooth_discovery_ref(y);

    y = pa_xnew0(pa_bluetooth_discovery, 1);
    PA_REFCNT_INIT(y);
    y->core = c;

    pa_shared_set(c, "bluetooth-discovery", y);

    return y;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    PA_REFCNT_INC(y);

    return y;
}

void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    if (PA_REFCNT_DEC(y) > 0)
        return;

    pa_shared_remove(y->core, "bluetooth-discovery");
    pa_xfree(y);
}
