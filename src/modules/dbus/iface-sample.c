/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

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

#include <pulsecore/core-util.h>
#include <pulsecore/protocol-dbus.h>

#include "iface-sample.h"

#define OBJECT_NAME "sample"

struct pa_dbusiface_sample {
    pa_scache_entry *sample;
    char *path;
};

pa_dbusiface_sample *pa_dbusiface_sample_new(pa_dbusiface_core *core, pa_scache_entry *sample) {
    pa_dbusiface_sample *s;

    pa_assert(core);
    pa_assert(sample);

    s = pa_xnew(pa_dbusiface_sample, 1);
    s->sample = sample;
    s->path = pa_sprintf_malloc("%s/%s%u", PA_DBUS_CORE_OBJECT_PATH, OBJECT_NAME, sample->index);

    return s;
}

void pa_dbusiface_sample_free(pa_dbusiface_sample *s) {
    pa_assert(s);

    pa_xfree(s->path);
    pa_xfree(s);
}

const char *pa_dbusiface_sample_get_path(pa_dbusiface_sample *s) {
    pa_assert(s);

    return s->path;
}
