/* $Id$ */

/***
  This file is part of PulseAudio.
 
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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/autoload.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/native-common.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/dynarray.h>
#include <pulsecore/modargs.h>
#include <pulsecore/avahi-wrap.h>
#include <pulsecore/endianmacros.h>

#include "module-zeroconf-publish-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("mDNS/DNS-SD Service Publisher")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("port=<IP port number>")

#define SERVICE_TYPE_SINK "_pulse-sink._tcp"
#define SERVICE_TYPE_SOURCE "_pulse-source._tcp"
#define SERVICE_TYPE_SERVER "_pulse-server._tcp"

static const char* const valid_modargs[] = {
    "port",
    NULL
};

struct service {
    struct userdata *userdata;
    AvahiEntryGroup *entry_group;
    char *service_name;
    char *name;
    enum  { UNPUBLISHED, PUBLISHED_REAL, PUBLISHED_AUTOLOAD } published ;

    struct {
        int valid;
        pa_namereg_type_t type;
        uint32_t index;
    } loaded;

    struct {
        int valid;
        pa_namereg_type_t type;
        uint32_t index;
    } autoload;
};

struct userdata {
    pa_core *core;
    AvahiPoll *avahi_poll;
    AvahiClient *client;
    pa_hashmap *services;
    pa_dynarray *sink_dynarray, *source_dynarray, *autoload_dynarray;
    pa_subscription *subscription;
    char *service_name;

    AvahiEntryGroup *main_entry_group;

    uint16_t port;
};

static void get_service_data(struct userdata *u, struct service *s, pa_sample_spec *ret_ss, char **ret_description) {
    assert(u && s && s->loaded.valid && ret_ss && ret_description);

    if (s->loaded.type == PA_NAMEREG_SINK) {
        pa_sink *sink = pa_idxset_get_by_index(u->core->sinks, s->loaded.index);
        assert(sink);
        *ret_ss = sink->sample_spec;
        *ret_description = sink->description;
    } else if (s->loaded.type == PA_NAMEREG_SOURCE) {
        pa_source *source = pa_idxset_get_by_index(u->core->sources, s->loaded.index);
        assert(source);
        *ret_ss = source->sample_spec;
        *ret_description = source->description;
    } else
        assert(0);
}

static AvahiStringList* txt_record_server_data(pa_core *c, AvahiStringList *l) {
    char s[128];
    assert(c);

    l = avahi_string_list_add_pair(l, "server-version", PACKAGE_NAME" "PACKAGE_VERSION);
    l = avahi_string_list_add_pair(l, "user-name", pa_get_user_name(s, sizeof(s)));
    l = avahi_string_list_add_pair(l, "fqdn", pa_get_fqdn(s, sizeof(s)));
    l = avahi_string_list_add_printf(l, "cookie=0x%08x", c->cookie);

    return l;
}

static int publish_service(struct userdata *u, struct service *s);

static void service_entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    struct service *s = userdata;

    if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        char *t;

        t = avahi_alternative_service_name(s->service_name);
        pa_xfree(s->service_name);
        s->service_name = t;

        publish_service(s->userdata, s);
    }
}

static int publish_service(struct userdata *u, struct service *s) {
    int r = -1;
    AvahiStringList *txt = NULL;

    assert(u);
    assert(s);

    if (!u->client || avahi_client_get_state(u->client) != AVAHI_CLIENT_S_RUNNING)
        return 0;
    
    if ((s->published == PUBLISHED_REAL && s->loaded.valid) ||
        (s->published == PUBLISHED_AUTOLOAD && s->autoload.valid && !s->loaded.valid))
        return 0;

    if (s->published != UNPUBLISHED) {
        avahi_entry_group_reset(s->entry_group);
        s->published = UNPUBLISHED;
    } 
    
    if (s->loaded.valid || s->autoload.valid) {
        pa_namereg_type_t type;

        if (!s->entry_group) {
            if (!(s->entry_group = avahi_entry_group_new(u->client, service_entry_group_callback, s))) {
                pa_log("avahi_entry_group_new(): %s", avahi_strerror(avahi_client_errno(u->client)));
                goto finish;
            }
        }
        
        txt = avahi_string_list_add_pair(txt, "device", s->name);
        txt = txt_record_server_data(u->core, txt);
        
        if (s->loaded.valid) {
            char *description;
            pa_sample_spec ss;
            
            get_service_data(u, s, &ss, &description);
            
            txt = avahi_string_list_add_printf(txt, "rate=%u", ss.rate);
            txt = avahi_string_list_add_printf(txt, "channels=%u", ss.channels);
            txt = avahi_string_list_add_pair(txt, "format", pa_sample_format_to_string(ss.format));
            if (description)
                txt = avahi_string_list_add_pair(txt, "description", description);
            
            type = s->loaded.type;
        } else if (s->autoload.valid)
            type = s->autoload.type;
        
        if (avahi_entry_group_add_service_strlst(
                    s->entry_group,
                    AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                    0,
                    s->service_name,
                    type == PA_NAMEREG_SINK ? SERVICE_TYPE_SINK : SERVICE_TYPE_SOURCE,
                    NULL,
                    NULL,
                    u->port,
                    txt) < 0) {
            
            pa_log(__FILE__": avahi_entry_group_add_service_strlst(): %s", avahi_strerror(avahi_client_errno(u->client)));
            goto finish;
        }
        
        if (avahi_entry_group_commit(s->entry_group) < 0) {
            pa_log(__FILE__": avahi_entry_group_commit(): %s", avahi_strerror(avahi_client_errno(u->client)));
            goto finish;
        }
        
        if (s->loaded.valid)
            s->published = PUBLISHED_REAL;
        else if (s->autoload.valid)
            s->published = PUBLISHED_AUTOLOAD;
    }
        
    r = 0;
    
finish:

    if (s->published == UNPUBLISHED) {
        /* Remove this service */

        if (s->entry_group)
            avahi_entry_group_free(s->entry_group);
        
        pa_hashmap_remove(u->services, s->name);
        pa_xfree(s->name);
        pa_xfree(s->service_name);
        pa_xfree(s);
    }

    if (txt)
        avahi_string_list_free(txt);
    
    return r;
}

static struct service *get_service(struct userdata *u, const char *name) {
    struct service *s;
    char hn[64];
    
    if ((s = pa_hashmap_get(u->services, name)))
        return s;
    
    s = pa_xnew(struct service, 1);
    s->userdata = u;
    s->entry_group = NULL;
    s->published = UNPUBLISHED;
    s->name = pa_xstrdup(name);
    s->loaded.valid = s->autoload.valid = 0;
    s->service_name = pa_sprintf_malloc("%s on %s", s->name, pa_get_host_name(hn, sizeof(hn)));

    pa_hashmap_put(u->services, s->name, s);

    return s;
}

static int publish_sink(struct userdata *u, pa_sink *s) {
    struct service *svc;
    int ret;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->loaded.valid)
        return publish_service(u, svc);

    svc->loaded.valid = 1;
    svc->loaded.type = PA_NAMEREG_SINK;
    svc->loaded.index = s->index;

    if ((ret = publish_service(u, svc)) < 0)
        return ret;

    pa_dynarray_put(u->sink_dynarray, s->index, svc);
    return ret;
}

static int publish_source(struct userdata *u, pa_source *s) {
    struct service *svc;
    int ret;
    
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->loaded.valid)
        return publish_service(u, svc);

    svc->loaded.valid = 1;
    svc->loaded.type = PA_NAMEREG_SOURCE;
    svc->loaded.index = s->index;

    pa_dynarray_put(u->source_dynarray, s->index, svc);
    
    if ((ret = publish_service(u, svc)) < 0)
        return ret;

    pa_dynarray_put(u->sink_dynarray, s->index, svc);
    return ret;
}

static int publish_autoload(struct userdata *u, pa_autoload_entry *s) {
    struct service *svc;
    int ret;
    
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->autoload.valid)
        return publish_service(u, svc);

    svc->autoload.valid = 1;
    svc->autoload.type = s->type;
    svc->autoload.index = s->index;

    if ((ret = publish_service(u, svc)) < 0)
        return ret;
    
    pa_dynarray_put(u->autoload_dynarray, s->index, svc);
    return ret;
}

static int remove_sink(struct userdata *u, uint32_t idx) {
    struct service *svc;
    assert(u && idx != PA_INVALID_INDEX);

    if (!(svc = pa_dynarray_get(u->sink_dynarray, idx)))
        return 0;

    if (!svc->loaded.valid || svc->loaded.type != PA_NAMEREG_SINK)
        return 0;

    svc->loaded.valid = 0;
    pa_dynarray_put(u->sink_dynarray, idx, NULL);
    
    return publish_service(u, svc);
}

static int remove_source(struct userdata *u, uint32_t idx) {
    struct service *svc;
    assert(u && idx != PA_INVALID_INDEX);
    
    if (!(svc = pa_dynarray_get(u->source_dynarray, idx)))
        return 0;

    if (!svc->loaded.valid || svc->loaded.type != PA_NAMEREG_SOURCE)
        return 0;

    svc->loaded.valid = 0;
    pa_dynarray_put(u->source_dynarray, idx, NULL);

    return publish_service(u, svc);
}

static int remove_autoload(struct userdata *u, uint32_t idx) {
    struct service *svc;
    assert(u && idx != PA_INVALID_INDEX);
    
    if (!(svc = pa_dynarray_get(u->autoload_dynarray, idx)))
        return 0;

    if (!svc->autoload.valid)
        return 0;

    svc->autoload.valid = 0;
    pa_dynarray_put(u->autoload_dynarray, idx, NULL);

    return publish_service(u, svc);
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    assert(u && c);

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
        case PA_SUBSCRIPTION_EVENT_SINK: {
            if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_sink *sink;

                if ((sink = pa_idxset_get_by_index(c->sinks, idx))) {
                    if (publish_sink(u, sink) < 0)
                        goto fail;
                }
            } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                if (remove_sink(u, idx) < 0)
                    goto fail;
            }
        
            break;

        case PA_SUBSCRIPTION_EVENT_SOURCE:

            if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_source *source;
                
                if ((source = pa_idxset_get_by_index(c->sources, idx))) {
                    if (publish_source(u, source) < 0)
                        goto fail;
                }
            } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                if (remove_source(u, idx) < 0)
                    goto fail;
            }
            
            break;

        case PA_SUBSCRIPTION_EVENT_AUTOLOAD:
            if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_autoload_entry *autoload;
                    
                if ((autoload = pa_idxset_get_by_index(c->autoload_idxset, idx))) {
                    if (publish_autoload(u, autoload) < 0)
                        goto fail;
                }
            } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                if (remove_autoload(u, idx) < 0)
                        goto fail;
            }
            
            break;
    }

    return;

fail:
    if (u->subscription) {
        pa_subscription_free(u->subscription);
        u->subscription = NULL;
    }
}

static int publish_main_service(struct userdata *u);

static void main_entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    struct userdata *u = userdata;
    assert(u);

    if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        char *t;

        t = avahi_alternative_service_name(u->service_name);
        pa_xfree(u->service_name);
        u->service_name = t;

        publish_main_service(u);
    }
}

static int publish_main_service(struct userdata *u) {
    AvahiStringList *txt = NULL;
    int r = -1;
    
    if (!u->main_entry_group) {
        if (!(u->main_entry_group = avahi_entry_group_new(u->client, main_entry_group_callback, u))) {
            pa_log(__FILE__": avahi_entry_group_new() failed: %s", avahi_strerror(avahi_client_errno(u->client)));
            goto fail;
        }
    } else
        avahi_entry_group_reset(u->main_entry_group);
    
    txt = txt_record_server_data(u->core, NULL);

    if (avahi_entry_group_add_service_strlst(
                u->main_entry_group,
                AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                0,
                u->service_name,
                SERVICE_TYPE_SERVER,
                NULL,
                NULL,
                u->port,
                txt) < 0) {
        
        pa_log(__FILE__": avahi_entry_group_add_service_strlst() failed: %s", avahi_strerror(avahi_client_errno(u->client)));
        goto fail;
    }
            
    if (avahi_entry_group_commit(u->main_entry_group) < 0) {
        pa_log(__FILE__": avahi_entry_group_commit() failed: %s", avahi_strerror(avahi_client_errno(u->client)));
        goto fail;
    }

    r = 0;
    
fail:
    avahi_string_list_free(txt);

    return r;
}

static int publish_all_services(struct userdata *u) {
    pa_sink *sink;
    pa_source *source;
    pa_autoload_entry *autoload;
    int r = -1;
    uint32_t idx;
    
    assert(u);

    pa_log_debug(__FILE__": Publishing services in Zeroconf");

    for (sink = pa_idxset_first(u->core->sinks, &idx); sink; sink = pa_idxset_next(u->core->sinks, &idx))
        if (publish_sink(u, sink) < 0)
            goto fail;

    for (source = pa_idxset_first(u->core->sources, &idx); source; source = pa_idxset_next(u->core->sources, &idx))
        if (publish_source(u, source) < 0)
            goto fail;

    if (u->core->autoload_idxset)
        for (autoload = pa_idxset_first(u->core->autoload_idxset, &idx); autoload; autoload = pa_idxset_next(u->core->autoload_idxset, &idx))
            if (publish_autoload(u, autoload) < 0)
                goto fail;

    if (publish_main_service(u) < 0)
        goto fail;
    
    r = 0;
    
fail:
    return r;
}

static void unpublish_all_services(struct userdata *u, int rem) {
    void *state = NULL;
    struct service *s;
    
    assert(u);

    pa_log_debug(__FILE__": Unpublishing services in Zeroconf");

    while ((s = pa_hashmap_iterate(u->services, &state, NULL))) {
        if (s->entry_group) {
            if (rem) {
                avahi_entry_group_free(s->entry_group);
                s->entry_group = NULL;
            } else 
                avahi_entry_group_reset(s->entry_group);
        }

        s->published = UNPUBLISHED;
    }

    if (u->main_entry_group) {
        if (rem) {
            avahi_entry_group_free(u->main_entry_group);
            u->main_entry_group = NULL;
        } else
            avahi_entry_group_reset(u->main_entry_group);
    }
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    struct userdata *u = userdata;
    assert(c);

    u->client = c;
    
    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            publish_all_services(u);
            break;
            
        case AVAHI_CLIENT_S_COLLISION:
            unpublish_all_services(u, 0);
            break;

        case AVAHI_CLIENT_FAILURE:
            if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
                int error;
                unpublish_all_services(u, 1);
                avahi_client_free(u->client);

                if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error)))
                    pa_log(__FILE__": pa_avahi_client_new() failed: %s", avahi_strerror(error));
            }
            
            break;

        default: ;
    }
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u;
    uint32_t port = PA_NATIVE_DEFAULT_PORT;
    pa_modargs *ma = NULL;
    char hn[256];
    int error;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port == 0 || port >= 0xFFFF) {
        pa_log(__FILE__": invalid port specified.");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = c;
    u->port = (uint16_t) port;

    u->avahi_poll = pa_avahi_poll_new(c->mainloop);
    
    u->services = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->sink_dynarray = pa_dynarray_new();
    u->source_dynarray = pa_dynarray_new();
    u->autoload_dynarray = pa_dynarray_new();

    u->subscription = pa_subscription_new(c,
                                          PA_SUBSCRIPTION_MASK_SINK|
                                          PA_SUBSCRIPTION_MASK_SOURCE|
                                          PA_SUBSCRIPTION_MASK_AUTOLOAD, subscribe_callback, u);

    u->main_entry_group = NULL;

    u->service_name = pa_xstrdup(pa_get_host_name(hn, sizeof(hn)));

    if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error))) {
        pa_log(__FILE__": pa_avahi_client_new() failed: %s", avahi_strerror(error));
        goto fail;
    }

    pa_modargs_free(ma);
    
    return 0;
    
fail:
    pa__done(c, m);

    if (ma)
        pa_modargs_free(ma);
    
    return -1;
}

static void service_free(void *p, void *userdata) {
    struct service *s = p;
    struct userdata *u = userdata;

    assert(s);
    assert(u);

    if (s->entry_group)
        avahi_entry_group_free(s->entry_group);
    
    pa_xfree(s->service_name);
    pa_xfree(s->name);
    pa_xfree(s);
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata*u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    if (u->services)
        pa_hashmap_free(u->services, service_free, u);

    if (u->subscription)
        pa_subscription_free(u->subscription);

    if (u->sink_dynarray)
        pa_dynarray_free(u->sink_dynarray, NULL, NULL);
    if (u->source_dynarray)
        pa_dynarray_free(u->source_dynarray, NULL, NULL);
    if (u->autoload_dynarray)
        pa_dynarray_free(u->autoload_dynarray, NULL, NULL);
    

    if (u->main_entry_group)
        avahi_entry_group_free(u->main_entry_group);
    
    if (u->client)
        avahi_client_free(u->client);
    
    if (u->avahi_poll)
        pa_avahi_poll_free(u->avahi_poll);

    pa_xfree(u->service_name);
    pa_xfree(u);
}

