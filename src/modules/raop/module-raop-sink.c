/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>

#include "raop-sink.h"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("RAOP Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "name=<name of the sink, to be prefixed> "
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> "
        "server=<address> "
        "protocol=<transport protocol> "
        "encryption=<encryption type> "
        "codec=<audio codec> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "username=<authentication user name, default: \"iTunes\"> "
        "password=<authentication password> "
        "latency_msec=<audio latency>");

static const char* const valid_modargs[] = {
    "name",
    "sink_name",
    "sink_properties",
    "server",
    "protocol",
    "encryption",
    "codec",
    "format",
    "rate",
    "channels",
    "channel_map",
    "username",
    "password",
    "latency_msec",
    "autoreconnect",
    NULL
};

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!(m->userdata = pa_raop_sink_new(m, ma, __FILE__)))
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

void pa__done(pa_module *m) {
    pa_sink *sink;

    pa_assert(m);

    if ((sink = m->userdata))
        pa_raop_sink_free(sink);
}
