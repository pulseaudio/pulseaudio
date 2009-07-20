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

#include "iface-device.h"

#define SINK_OBJECT_NAME "sink"
#define SOURCE_OBJECT_NAME "source"

enum device_type {
    DEVICE_TYPE_SINK,
    DEVICE_TYPE_SOURCE
};

struct pa_dbusiface_device {
    union {
        pa_sink *sink;
        pa_source *source;
    };
    enum device_type type;
    char *path;
};

pa_dbusiface_device *pa_dbusiface_device_new_sink(pa_sink *sink, const char *path_prefix) {
    pa_dbusiface_device *d;

    pa_assert(sink);
    pa_assert(path_prefix);

    d = pa_xnew(pa_dbusiface_device, 1);
    d->sink = pa_sink_ref(sink);
    d->type = DEVICE_TYPE_SINK;
    d->path = pa_sprintf_malloc("%s/%s%u", path_prefix, SINK_OBJECT_NAME, sink->index);

    return d;
}

pa_dbusiface_device *pa_dbusiface_device_new_source(pa_source *source, const char *path_prefix) {
    pa_dbusiface_device *d;

    pa_assert(source);
    pa_assert(path_prefix);

    d = pa_xnew(pa_dbusiface_device, 1);
    d->source = pa_source_ref(source);
    d->type = DEVICE_TYPE_SOURCE;
    d->path = pa_sprintf_malloc("%s/%s%u", path_prefix, SOURCE_OBJECT_NAME, source->index);

    return d;
}

void pa_dbusiface_device_free(pa_dbusiface_device *d) {
    pa_assert(d);

    if (d->type == DEVICE_TYPE_SINK)
        pa_sink_unref(d->sink);
    else
        pa_source_unref(d->source);

    pa_xfree(d->path);
    pa_xfree(d);
}

const char *pa_dbusiface_device_get_path(pa_dbusiface_device *d) {
    pa_assert(d);

    return d->path;
}

pa_sink *pa_dbusiface_device_get_sink(pa_dbusiface_device *d) {
    pa_assert(d);
    pa_assert(d->type == DEVICE_TYPE_SINK);

    return d->sink;
}

pa_source *pa_dbusiface_device_get_source(pa_dbusiface_device *d) {
    pa_assert(d);
    pa_assert(d->type == DEVICE_TYPE_SOURCE);

    return d->source;
}
