/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
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

#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/avahi-wrap.h>

#include "raop-util.h"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("mDNS/DNS-SD Service Discovery of RAOP devices");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "latency_msec=<audio latency - applies to all devices> ");

#define SERVICE_TYPE_SINK "_raop._tcp"

struct userdata {
    pa_core *core;
    pa_module *module;

    AvahiPoll *avahi_poll;
    AvahiClient *client;
    AvahiServiceBrowser *sink_browser;

    pa_hashmap *tunnels;

    bool latency_set;
    uint32_t latency;
};

static const char* const valid_modargs[] = {
    "latency_msec",
    NULL
};

struct tunnel {
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char *name, *type, *domain;
    uint32_t module_index;
};

static unsigned tunnel_hash(const void *p) {
    const struct tunnel *t = p;

    return
        (unsigned) t->interface +
        (unsigned) t->protocol +
        pa_idxset_string_hash_func(t->name) +
        pa_idxset_string_hash_func(t->type) +
        pa_idxset_string_hash_func(t->domain);
}

static int tunnel_compare(const void *a, const void *b) {
    const struct tunnel *ta = a, *tb = b;
    int r;

    if (ta->interface != tb->interface)
        return 1;
    if (ta->protocol != tb->protocol)
        return 1;
    if ((r = strcmp(ta->name, tb->name)))
        return r;
    if ((r = strcmp(ta->type, tb->type)))
        return r;
    if ((r = strcmp(ta->domain, tb->domain)))
        return r;

    return 0;
}

static struct tunnel* tunnel_new(
        AvahiIfIndex interface, AvahiProtocol protocol,
        const char *name, const char *type, const char *domain) {
    struct tunnel *t;

    t = pa_xnew(struct tunnel, 1);
    t->interface = interface;
    t->protocol = protocol;
    t->name = pa_xstrdup(name);
    t->type = pa_xstrdup(type);
    t->domain = pa_xstrdup(domain);
    t->module_index = PA_IDXSET_INVALID;

    return t;
}

static void tunnel_free(struct tunnel *t) {
    pa_assert(t);
    pa_xfree(t->name);
    pa_xfree(t->type);
    pa_xfree(t->domain);
    pa_xfree(t);
}

/* This functions returns RAOP audio latency as guessed by the
 * device model header.
 * Feel free to complete the possible values after testing with
 * your hardware.
 */
static uint32_t guess_latency_from_device(const char *model) {
    uint32_t default_latency = RAOP_DEFAULT_LATENCY;

    if (pa_streq(model, "PIONEER,1")) {
        /* Pioneer N-30 */
        default_latency = 2352;
    } else if (pa_streq(model, "ShairportSync")) {
        /* Shairport - software AirPort server */
        default_latency = 2352;
    }

    pa_log_debug("Default latency is %u ms for device model %s.", default_latency, model);
    return default_latency;
}

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
    struct tunnel *tnl;
    char *device = NULL, *nicename, *dname, *vname, *args;
    char *tp = NULL, *et = NULL, *cn = NULL;
    char *ch = NULL, *ss = NULL, *sr = NULL;
    char *dm = NULL;
    char *t = NULL;
    char at[AVAHI_ADDRESS_STR_MAX];
    AvahiStringList *l;
    pa_module *m;
    uint32_t latency = RAOP_DEFAULT_LATENCY;

    pa_assert(u);

    tnl = tunnel_new(interface, protocol, name, type, domain);

    if (event != AVAHI_RESOLVER_FOUND) {
        pa_log("Resolving of '%s' failed: %s", name, avahi_strerror(avahi_client_errno(u->client)));
        goto finish;
    }

    if ((nicename = strstr(name, "@"))) {
        ++nicename;
        if (strlen(nicename) > 0) {
            pa_log_debug("Found RAOP: %s", nicename);
            nicename = pa_escape(nicename, "\"'");
        } else
            nicename = NULL;
    }

    for (l = txt; l; l = l->next) {
        char *key, *value;
        pa_assert_se(avahi_string_list_get_pair(l, &key, &value, NULL) == 0);

        pa_log_debug("Found key: '%s' with value: '%s'", key, value);
        if (pa_streq(key, "device")) {
            device = value;
            value = NULL;
        } else if (pa_streq(key, "tp")) {
            /* Transport protocol:
             *  - TCP = only TCP,
             *  - UDP = only UDP,
             *  - TCP,UDP = both supported (UDP should be preferred) */
            pa_xfree(tp);
            if (pa_str_in_list(value, ",", "UDP"))
                tp = pa_xstrdup("UDP");
            else if (pa_str_in_list(value, ",", "TCP"))
                tp = pa_xstrdup("TCP");
            else
                tp = pa_xstrdup(value);
        } else if (pa_streq(key, "et")) {
            /* Supported encryption types:
             *  - 0 = none,
             *  - 1 = RSA,
             *  - 2 = FairPlay,
             *  - 3 = MFiSAP,
             *  - 4 = FairPlay SAPv2.5. */
            pa_xfree(et);
            if (pa_str_in_list(value, ",", "1"))
                et = pa_xstrdup("RSA");
            else
                et = pa_xstrdup("none");
        } else if (pa_streq(key, "cn")) {
            /* Suported audio codecs:
             *  - 0 = PCM,
             *  - 1 = ALAC,
             *  - 2 = AAC,
             *  - 3 = AAC ELD. */
            pa_xfree(cn);
            if (pa_str_in_list(value, ",", "1"))
                cn = pa_xstrdup("ALAC");
            else
                cn = pa_xstrdup("PCM");
        } else if (pa_streq(key, "md")) {
            /* Supported metadata types:
             *  - 0 = text,
             *  - 1 = artwork,
             *  - 2 = progress. */
        } else if (pa_streq(key, "pw")) {
            /* Requires password ? (true/false) */
        } else if (pa_streq(key, "ch")) {
            /* Number of channels */
            pa_xfree(ch);
            ch = pa_xstrdup(value);
        } else if (pa_streq(key, "ss")) {
            /* Sample size */
            pa_xfree(ss);
            ss = pa_xstrdup(value);
        } else if (pa_streq(key, "sr")) {
            /* Sample rate */
            pa_xfree(sr);
            sr = pa_xstrdup(value);
        } else if (pa_streq(key, "am")) {
            /* Device model */
            pa_xfree(dm);
            dm = pa_xstrdup(value);
        }

        avahi_free(key);
        avahi_free(value);
    }

    if (device)
        dname = pa_sprintf_malloc("raop_output.%s.%s", host_name, device);
    else
        dname = pa_sprintf_malloc("raop_output.%s", host_name);

    if (!(vname = pa_namereg_make_valid_name(dname))) {
        pa_log("Cannot construct valid device name from '%s'.", dname);
        avahi_free(device);
        pa_xfree(dname);
        pa_xfree(tp);
        pa_xfree(et);
        pa_xfree(cn);
        pa_xfree(ch);
        pa_xfree(ss);
        pa_xfree(sr);
        pa_xfree(dm);
        goto finish;
    }

    avahi_free(device);
    pa_xfree(dname);

    avahi_address_snprint(at, sizeof(at), a);

    if (nicename == NULL)
        nicename = pa_xstrdup("RAOP");

    if (dm == NULL)
        dm = pa_xstrdup(_("Unknown device model"));

    latency = guess_latency_from_device(dm);

    args = pa_sprintf_malloc("server=[%s]:%u "
                             "sink_name=%s "
                             "sink_properties='device.description=\"%s\" device.model=\"%s\"'",
                             at, port,
                             vname,
                             nicename,
                             dm);
    pa_xfree(nicename);
    pa_xfree(dm);

    if (tp != NULL) {
        t = args;
        args = pa_sprintf_malloc("%s protocol=%s", args, tp);
        pa_xfree(tp);
        pa_xfree(t);
    }
    if (et != NULL) {
        t = args;
        args = pa_sprintf_malloc("%s encryption=%s", args, et);
        pa_xfree(et);
        pa_xfree(t);
    }
    if (cn != NULL) {
        t = args;
        args = pa_sprintf_malloc("%s codec=%s", args, cn);
        pa_xfree(cn);
        pa_xfree(t);
    }
    if (ch != NULL) {
        t = args;
        args = pa_sprintf_malloc("%s channels=%s", args, ch);
        pa_xfree(ch);
        pa_xfree(t);
    }
    if (ss != NULL) {
        t = args;
        args = pa_sprintf_malloc("%s format=%s", args, ss);
        pa_xfree(ss);
        pa_xfree(t);
    }
    if (sr != NULL) {
        t = args;
        args = pa_sprintf_malloc("%s rate=%s", args, sr);
        pa_xfree(sr);
        pa_xfree(t);
    }

    if (u->latency_set)
        latency = u->latency;

    t = args;
    args = pa_sprintf_malloc("%s latency_msec=%u", args, latency);
    pa_xfree(t);

    pa_log_debug("Loading module-raop-sink with arguments '%s'", args);

    if (pa_module_load(&m, u->core, "module-raop-sink", args) >= 0) {
        tnl->module_index = m->index;
        pa_hashmap_put(u->tunnels, tnl, tnl);
        tnl = NULL;
    }

    pa_xfree(vname);
    pa_xfree(args);

finish:
    avahi_service_resolver_free(r);

    if (tnl)
        tunnel_free(tnl);
}

static void browser_cb(
        AvahiServiceBrowser *b,
        AvahiIfIndex interface, AvahiProtocol protocol,
        AvahiBrowserEvent event,
        const char *name, const char *type, const char *domain,
        AvahiLookupResultFlags flags,
        void *userdata) {
    struct userdata *u = userdata;
    struct tunnel *t;

    pa_assert(u);

    if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
        return;

    t = tunnel_new(interface, protocol, name, type, domain);

    if (event == AVAHI_BROWSER_NEW) {

        if (!pa_hashmap_get(u->tunnels, t))
            if (!(avahi_service_resolver_new(u->client, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolver_cb, u)))
                pa_log("avahi_service_resolver_new() failed: %s", avahi_strerror(avahi_client_errno(u->client)));

        /* We ignore the returned resolver object here, since the we don't
         * need to attach any special data to it, and we can still destroy
         * it from the callback. */

    } else if (event == AVAHI_BROWSER_REMOVE) {
        struct tunnel *t2;

        if ((t2 = pa_hashmap_get(u->tunnels, t))) {
            pa_module_unload_request_by_index(u->core, t2->module_index, true);
            pa_hashmap_remove(u->tunnels, t2);
            tunnel_free(t2);
        }
    }

    tunnel_free(t);
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
                    pa_module_unload_request(u->module, true);
                }
            }

            break;

        case AVAHI_CLIENT_FAILURE:
            if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
                int error;

                pa_log_debug("Avahi daemon disconnected.");

                /* Try to reconnect. */
                if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error))) {
                    pa_log("avahi_client_new() failed: %s", avahi_strerror(error));
                    pa_module_unload_request(u->module, true);
                }
            }

            /* Fall through. */

        case AVAHI_CLIENT_CONNECTING:
            if (u->sink_browser) {
                avahi_service_browser_free(u->sink_browser);
                u->sink_browser = NULL;
            }

            break;

        default:
            break;
    }
}

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    int error;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;

    if (pa_modargs_get_value(ma, "latency_msec", NULL) != NULL) {
        u->latency_set = true;
        if (pa_modargs_get_value_u32(ma, "latency_msec", &u->latency) < 0) {
            pa_log("Failed to parse latency_msec argument.");
            goto fail;
        }
    }

    u->tunnels = pa_hashmap_new(tunnel_hash, tunnel_compare);

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

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->client)
        avahi_client_free(u->client);

    if (u->avahi_poll)
        pa_avahi_poll_free(u->avahi_poll);

    if (u->tunnels) {
        struct tunnel *t;

        while ((t = pa_hashmap_steal_first(u->tunnels))) {
            pa_module_unload_request_by_index(u->core, t->module_index, true);
            tunnel_free(t);
        }

        pa_hashmap_free(u->tunnels);
    }

    pa_xfree(u);
}
