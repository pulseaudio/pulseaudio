/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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
#include "config.h"
#endif

#include <string.h>

#include <avahi-client/lookup.h>
#include <avahi-common/domain.h>
#include <avahi-common/error.h>

#include <pulse/xmalloc.h>

#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/avahi-wrap.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/macro.h>

#include "browser.h"

#define SERVICE_TYPE_SINK "_pulse-sink._tcp."
#define SERVICE_TYPE_SOURCE "_pulse-source._tcp."
#define SERVICE_TYPE_SERVER "_pulse-server._tcp."

struct pa_browser {
    PA_REFCNT_DECLARE;

    pa_mainloop_api *mainloop;
    AvahiPoll* avahi_poll;

    pa_browse_cb_t callback;
    void *userdata;

    pa_browser_error_cb_t error_callback;
    void *error_userdata;

    AvahiClient *client;
    AvahiServiceBrowser *server_browser, *sink_browser, *source_browser;

};

static int map_to_opcode(const char *type, int new) {

    if (avahi_domain_equal(type, SERVICE_TYPE_SINK))
        return new ? PA_BROWSE_NEW_SINK : PA_BROWSE_REMOVE_SINK;
    else if (avahi_domain_equal(type, SERVICE_TYPE_SOURCE))
        return new ? PA_BROWSE_NEW_SOURCE : PA_BROWSE_REMOVE_SOURCE;
    else if (avahi_domain_equal(type, SERVICE_TYPE_SERVER))
        return new ? PA_BROWSE_NEW_SERVER : PA_BROWSE_REMOVE_SERVER;

    return -1;
}

static void resolve_callback(
        AvahiServiceResolver *r,
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        AvahiResolverEvent event,
        const char *name,
        const char *type,
        const char *domain,
        const char *host_name,
        const AvahiAddress *aa,
        uint16_t port,
        AvahiStringList *txt,
        AvahiLookupResultFlags flags,
        void *userdata) {

    pa_browser *b = userdata;
    pa_browse_info i;
    char ip[256], a[256];
    int opcode;
    int device_found = 0;
    uint32_t cookie;
    pa_sample_spec ss;
    int ss_valid = 0;
    char *key = NULL, *value = NULL;

    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    memset(&i, 0, sizeof(i));
    i.name = name;

    if (event != AVAHI_RESOLVER_FOUND)
        goto fail;

    if (!b->callback)
        goto fail;

    opcode = map_to_opcode(type, 1);
    pa_assert(opcode >= 0);

    if (aa->proto == AVAHI_PROTO_INET)
        pa_snprintf(a, sizeof(a), "tcp:%s:%u", avahi_address_snprint(ip, sizeof(ip), aa), port);
    else {
        pa_assert(aa->proto == AVAHI_PROTO_INET6);
        pa_snprintf(a, sizeof(a), "tcp6:%s:%u", avahi_address_snprint(ip, sizeof(ip), aa), port);
    }
    i.server = a;


    while (txt) {

        if (avahi_string_list_get_pair(txt, &key, &value, NULL) < 0)
            break;

        if (!strcmp(key, "device")) {
            device_found = 1;
            pa_xfree((char*) i.device);
            i.device = value;
            value = NULL;
        } else if (!strcmp(key, "server-version")) {
            pa_xfree((char*) i.server_version);
            i.server_version = value;
            value = NULL;
        } else if (!strcmp(key, "user-name")) {
            pa_xfree((char*) i.user_name);
            i.user_name = value;
            value = NULL;
        } else if (!strcmp(key, "fqdn")) {
            size_t l;

            pa_xfree((char*) i.fqdn);
            i.fqdn = value;
            value = NULL;

            l = strlen(a);
            pa_assert(l+1 <= sizeof(a));
            strncat(a, " ", sizeof(a)-l-1);
            strncat(a, i.fqdn, sizeof(a)-l-2);
        } else if (!strcmp(key, "cookie")) {

            if (pa_atou(value, &cookie) < 0)
                goto fail;

            i.cookie = &cookie;
        } else if (!strcmp(key, "description")) {
            pa_xfree((char*) i.description);
            i.description = value;
            value = NULL;
        } else if (!strcmp(key, "channels")) {
            uint32_t ch;

            if (pa_atou(value, &ch) < 0 || ch <= 0 || ch > 255)
                goto fail;

            ss.channels = (uint8_t) ch;
            ss_valid |= 1;

        } else if (!strcmp(key, "rate")) {
            if (pa_atou(value, &ss.rate) < 0)
                goto fail;
            ss_valid |= 2;
        } else if (!strcmp(key, "format")) {

            if ((ss.format = pa_parse_sample_format(value)) == PA_SAMPLE_INVALID)
                goto fail;

            ss_valid |= 4;
        }

        pa_xfree(key);
        pa_xfree(value);
        key = value = NULL;

        txt = avahi_string_list_get_next(txt);
    }

    /* No device txt record was sent for a sink or source service */
    if (opcode != PA_BROWSE_NEW_SERVER && !device_found)
        goto fail;

    if (ss_valid == 7)
        i.sample_spec = &ss;

    b->callback(b, opcode, &i, b->userdata);

fail:
    pa_xfree((void*) i.device);
    pa_xfree((void*) i.fqdn);
    pa_xfree((void*) i.server_version);
    pa_xfree((void*) i.user_name);
    pa_xfree((void*) i.description);

    pa_xfree(key);
    pa_xfree(value);

    avahi_service_resolver_free(r);
}

static void handle_failure(pa_browser *b) {
    const char *e = NULL;

    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    if (b->sink_browser)
        avahi_service_browser_free(b->sink_browser);
    if (b->source_browser)
        avahi_service_browser_free(b->source_browser);
    if (b->server_browser)
        avahi_service_browser_free(b->server_browser);

    b->sink_browser = b->source_browser = b->server_browser = NULL;

    if (b->client) {
        e = avahi_strerror(avahi_client_errno(b->client));
        avahi_client_free(b->client);
    }

    b->client = NULL;

    if (b->error_callback)
        b->error_callback(b, e, b->error_userdata);
}

static void browse_callback(
        AvahiServiceBrowser *sb,
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        AvahiBrowserEvent event,
        const char *name,
        const char *type,
        const char *domain,
        AvahiLookupResultFlags flags,
        void *userdata) {

    pa_browser *b = userdata;

    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    switch (event) {
        case AVAHI_BROWSER_NEW: {

            if (!avahi_service_resolver_new(
                          b->client,
                          interface,
                          protocol,
                          name,
                          type,
                          domain,
                          AVAHI_PROTO_UNSPEC,
                          0,
                          resolve_callback,
                          b))
                handle_failure(b);

            break;
        }

        case AVAHI_BROWSER_REMOVE: {

            if (b->callback) {
                pa_browse_info i;
                int opcode;

                memset(&i, 0, sizeof(i));
                i.name = name;

                opcode = map_to_opcode(type, 0);
                pa_assert(opcode >= 0);

                b->callback(b, opcode, &i, b->userdata);
            }
            break;
        }

        case AVAHI_BROWSER_FAILURE: {
            handle_failure(b);
            break;
        }

        default:
            ;
    }
}

static void client_callback(AvahiClient *s, AvahiClientState state, void *userdata) {
    pa_browser *b = userdata;

    pa_assert(s);
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    if (state == AVAHI_CLIENT_FAILURE)
        handle_failure(b);
}

static void browser_free(pa_browser *b);


PA_WARN_REFERENCE(pa_browser_new, "libpulse-browse is being phased out.");

pa_browser *pa_browser_new(pa_mainloop_api *mainloop) {
    return pa_browser_new_full(mainloop, PA_BROWSE_FOR_SERVERS|PA_BROWSE_FOR_SINKS|PA_BROWSE_FOR_SOURCES, NULL);
}

PA_WARN_REFERENCE(pa_browser_new_full, "libpulse-browse is being phased out.");

pa_browser *pa_browser_new_full(pa_mainloop_api *mainloop, pa_browse_flags_t flags, const char **error_string) {
    pa_browser *b;
    int error;

    pa_assert(mainloop);

    if (flags & ~(PA_BROWSE_FOR_SERVERS|PA_BROWSE_FOR_SINKS|PA_BROWSE_FOR_SOURCES) || flags == 0)
        return NULL;

    b = pa_xnew(pa_browser, 1);
    b->mainloop = mainloop;
    PA_REFCNT_INIT(b);
    b->callback = NULL;
    b->userdata = NULL;
    b->error_callback = NULL;
    b->error_userdata = NULL;
    b->sink_browser = b->source_browser = b->server_browser = NULL;

    b->avahi_poll = pa_avahi_poll_new(mainloop);

    if (!(b->client = avahi_client_new(b->avahi_poll, 0, client_callback, b, &error))) {
        if (error_string)
            *error_string = avahi_strerror(error);
        goto fail;
    }

    if ((flags & PA_BROWSE_FOR_SERVERS) &&
        !(b->server_browser = avahi_service_browser_new(
                  b->client,
                  AVAHI_IF_UNSPEC,
                  AVAHI_PROTO_INET,
                  SERVICE_TYPE_SERVER,
                  NULL,
                  0,
                  browse_callback,
                  b))) {

        if (error_string)
            *error_string = avahi_strerror(avahi_client_errno(b->client));
        goto fail;
    }

    if ((flags & PA_BROWSE_FOR_SINKS) &&
        !(b->sink_browser = avahi_service_browser_new(
                  b->client,
                  AVAHI_IF_UNSPEC,
                  AVAHI_PROTO_UNSPEC,
                  SERVICE_TYPE_SINK,
                  NULL,
                  0,
                  browse_callback,
                  b))) {

        if (error_string)
            *error_string = avahi_strerror(avahi_client_errno(b->client));
        goto fail;
    }

    if ((flags & PA_BROWSE_FOR_SOURCES) &&
        !(b->source_browser = avahi_service_browser_new(
                  b->client,
                  AVAHI_IF_UNSPEC,
                  AVAHI_PROTO_UNSPEC,
                  SERVICE_TYPE_SOURCE,
                  NULL,
                  0,
                  browse_callback,
                  b))) {

        if (error_string)
            *error_string = avahi_strerror(avahi_client_errno(b->client));
        goto fail;
    }

    return b;

fail:
    if (b)
        browser_free(b);

    return NULL;
}

static void browser_free(pa_browser *b) {
    pa_assert(b);
    pa_assert(b->mainloop);

    if (b->sink_browser)
        avahi_service_browser_free(b->sink_browser);
    if (b->source_browser)
        avahi_service_browser_free(b->source_browser);
    if (b->server_browser)
        avahi_service_browser_free(b->server_browser);

    if (b->client)
        avahi_client_free(b->client);

    if (b->avahi_poll)
        pa_avahi_poll_free(b->avahi_poll);

    pa_xfree(b);
}

PA_WARN_REFERENCE(pa_browser_ref, "libpulse-browse is being phased out.");

pa_browser *pa_browser_ref(pa_browser *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    PA_REFCNT_INC(b);
    return b;
}

PA_WARN_REFERENCE(pa_browser_unref, "libpulse-browse is being phased out.");

void pa_browser_unref(pa_browser *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    if (PA_REFCNT_DEC(b) <= 0)
        browser_free(b);
}

PA_WARN_REFERENCE(pa_browser_set_callback, "libpulse-browse is being phased out.");

void pa_browser_set_callback(pa_browser *b, pa_browse_cb_t cb, void *userdata) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    b->callback = cb;
    b->userdata = userdata;
}

PA_WARN_REFERENCE(pa_browser_set_error_callback, "libpulse-browse is being phased out.");

void pa_browser_set_error_callback(pa_browser *b, pa_browser_error_cb_t cb, void *userdata) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) >= 1);

    b->error_callback = cb;
    b->error_userdata = userdata;
}
