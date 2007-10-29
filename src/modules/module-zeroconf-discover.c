/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/domain.h>
#include <avahi-common/malloc.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/native-common.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/dynarray.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/avahi-wrap.h>

#include "module-zeroconf-discover-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("mDNS/DNS-SD Service Discovery")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define SERVICE_TYPE_SINK "_pulse-sink._tcp"
#define SERVICE_TYPE_SOURCE "_non-monitor._sub._pulse-source._tcp"

 static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    AvahiPoll *avahi_poll;
    AvahiClient *client;
    AvahiServiceBrowser *source_browser, *sink_browser;
};

static void resolver_cb(
        AvahiServiceResolver *r,
        AvahiIfIndex interface, AvahiProtocol protocol,
        AvahiResolverEvent event,
        const char *name, const char *type, const char *domain,
        const char *host_name, const AvahiAddress *a, uint16_t port,
        AvahiStringList *txt,
        AvahiLookupResultFlags flags,
        void *userdata) {

    struct userdata *u = userdata;

    pa_assert(u);

    if (event != AVAHI_RESOLVER_FOUND)
        pa_log("Resolving of '%s' failed: %s", name, avahi_strerror(avahi_client_errno(u->client)));
    else {
        char *device = NULL, *dname, *module_name, *args;
        const char *t;
        char at[AVAHI_ADDRESS_STR_MAX], cmt[PA_CHANNEL_MAP_SNPRINT_MAX];
        pa_sample_spec ss;
        pa_channel_map cm;
        AvahiStringList *l;
        pa_bool_t channel_map_set = FALSE;

        ss = u->core->default_sample_spec;
        pa_channel_map_init_auto(&cm, ss.channels, PA_CHANNEL_MAP_DEFAULT);

        for (l = txt; l; l = l->next) {
            char *key, *value;
            pa_assert_se(avahi_string_list_get_pair(l, &key, &value, NULL) == 0);

            if (strcmp(key, "device") == 0) {
                pa_xfree(device);
                device = value;
                value = NULL;
            } else if (strcmp(key, "rate") == 0)
                ss.rate = atoi(value);
            else if (strcmp(key, "channels") == 0)
                ss.channels = atoi(value);
            else if (strcmp(key, "format") == 0)
                ss.format = pa_parse_sample_format(value);
            else if (strcmp(key, "channel_map") == 0) {
                pa_channel_map_parse(&cm, value);
                channel_map_set = TRUE;
            }

            avahi_free(key);
            avahi_free(value);
        }

        if (!channel_map_set && cm.channels != ss.channels)
            pa_channel_map_init_auto(&cm, ss.channels, PA_CHANNEL_MAP_DEFAULT);

        if (!pa_sample_spec_valid(&ss)) {
            pa_log("Service '%s' contains an invalid sample specification.", name);
            avahi_free(device);
            goto finish;
        }

        if (!pa_channel_map_valid(&cm) || cm.channels != ss.channels) {
            pa_log("Service '%s' contains an invalid channel map.", name);
            avahi_free(device);
            goto finish;
        }

        if (device)
            dname = pa_sprintf_malloc("tunnel.%s.%s", host_name, device);
        else
            dname = pa_sprintf_malloc("tunnel.%s", host_name);

        if (!pa_namereg_is_valid_name(dname)) {
            pa_log("Cannot construct valid device name from credentials of service '%s'.", dname);
            avahi_free(device);
            pa_xfree(dname);
            goto finish;
        }

        t = strstr(type, "sink") ? "sink" : "source";

        module_name = pa_sprintf_malloc("module-tunnel-%s", t);
        args = pa_sprintf_malloc("server=[%s]:%u "
                                 "%s=%s "
                                 "format=%s "
                                 "channels=%u "
                                 "rate=%u "
                                 "%s_name=%s "
                                 "channel_map=%s",
                                 avahi_address_snprint(at, sizeof(at), a), port,
                                 t, device,
                                 pa_sample_format_to_string(ss.format),
                                 ss.channels,
                                 ss.rate,
                                 t, dname,
                                 pa_channel_map_snprint(cmt, sizeof(cmt), &cm));

        pa_log_debug("Loading module-tunnel-%s with arguments '%s'", module_name, args);
        pa_module_load(u->core, module_name, args);

        pa_xfree(module_name);
        pa_xfree(dname);
        pa_xfree(args);
        avahi_free(device);
    }

finish:

    avahi_service_resolver_free(r);
}

static void browser_cb(
        AvahiServiceBrowser *b,
        AvahiIfIndex interface, AvahiProtocol protocol,
        AvahiBrowserEvent event,
        const char *name, const char *type, const char *domain,
        AvahiLookupResultFlags flags,
        void *userdata) {

    struct userdata *u = userdata;

    pa_assert(u);

    if (event != AVAHI_BROWSER_NEW)
        return;

    if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
        return;

    if (!(avahi_service_resolver_new(u->client, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolver_cb, u)))
        pa_log("avahi_service_resolver_new() failed: %s", avahi_strerror(avahi_client_errno(u->client)));

    /* We ignore the returned resolver object here, since the we don't
     * need to attach any special data to it, and we can still destory
     * it from the callback */
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(u);

    u->client = c;

    switch (state) {
        case AVAHI_CLIENT_S_REGISTERING:
        case AVAHI_CLIENT_S_RUNNING:
        case AVAHI_CLIENT_S_COLLISION:

            if (!u->sink_browser) {

                if (!(u->sink_browser = avahi_service_browser_new(
                              c,
                              AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                              SERVICE_TYPE_SINK,
                              NULL,
                              0,
                              browser_cb, u))) {

                    pa_log("avahi_service_browser_new() failed: %s", avahi_strerror(avahi_client_errno(c)));
                    pa_module_unload_request(u->module);
                }
            }

            if (!u->source_browser) {

                if (!(u->source_browser = avahi_service_browser_new(
                              c,
                              AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                              SERVICE_TYPE_SOURCE,
                              NULL,
                              0,
                              browser_cb, u))) {

                    pa_log("avahi_service_browser_new() failed: %s", avahi_strerror(avahi_client_errno(c)));
                    pa_module_unload_request(u->module);
                }
            }

            break;

        case AVAHI_CLIENT_FAILURE:
            if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
                int error;

                pa_log_debug("Avahi daemon disconnected.");

                if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error))) {
                    pa_log("avahi_client_new() failed: %s", avahi_strerror(error));
                    pa_module_unload_request(u->module);
                }
            }

            /* Fall through */

        case AVAHI_CLIENT_CONNECTING:

            if (u->sink_browser) {
                avahi_service_browser_free(u->sink_browser);
                u->sink_browser = NULL;
            }

            if (u->source_browser) {
                avahi_service_browser_free(u->source_browser);
                u->source_browser = NULL;
            }

            break;

        default: ;
    }
}

int pa__init(pa_module*m) {

    struct userdata *u;
    pa_modargs *ma = NULL;
    int error;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->sink_browser = u->source_browser = NULL;

    u->avahi_poll = pa_avahi_poll_new(m->core->mainloop);

    if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error))) {
        pa_log("pa_avahi_client_new() failed: %s", avahi_strerror(error));
        goto fail;
    }

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata*u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->client)
        avahi_client_free(u->client);

    if (u->avahi_poll)
        pa_avahi_poll_free(u->avahi_poll);

    pa_xfree(u);
}
