/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <pulsecore/core.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>

#include "alsa-util.h"
#include "alsa-sink.h"
#include "module-alsa-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "name=<name of the sink, to be prefixed> "
        "sink_name=<name for the sink> "
        "sink_properities=<properties for the sink> "
        "device=<ALSA device> "
        "device_id=<ALSA card index> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "mmap=<enable memory mapping?> "
        "tsched=<enable system timer based scheduling mode?> "
        "tsched_buffer_size=<buffer size when using timer based scheduling> "
        "tsched_buffer_watermark=<lower fill watermark> "
        "ignore_dB=<ignore dB information from the device?> "
        "control=<name of mixer control>");

static const char* const valid_modargs[] = {
    "name",
    "sink_name",
    "sink_properties",
    "device",
    "device_id",
    "format",
    "rate",
    "channels",
    "channel_map",
    "fragments",
    "fragment_size",
    "mmap",
    "tsched",
    "tsched_buffer_size",
    "tsched_buffer_watermark",
    "ignore_dB",
    "control",
    NULL
};

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;

    pa_assert(m);

    pa_alsa_refcnt_inc();

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!(m->userdata = pa_alsa_sink_new(m, ma, __FILE__, NULL, NULL)))
        goto fail;

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    pa_sink *sink;

    pa_assert(m);
    pa_assert_se(sink = m->userdata);

    return pa_sink_linked_by(sink);
}

void pa__done(pa_module*m) {
    pa_sink *sink;

    pa_assert(m);

    if ((sink = m->userdata))
        pa_alsa_sink_free(sink);

    pa_alsa_refcnt_dec();
}
