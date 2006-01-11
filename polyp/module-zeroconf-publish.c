/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
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

#include "module-zeroconf-publish-symdef.h"
#include "howl-wrap.h"
#include "xmalloc.h"
#include "autoload.h"
#include "sink.h"
#include "source.h"
#include "native-common.h"
#include "util.h"
#include "log.h"
#include "subscribe.h"
#include "dynarray.h"
#include "endianmacros.h"
#include "modargs.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("mDNS/DNS-SD Service Publisher")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("port=<IP port number>")

#define SERVICE_NAME_SINK "_polypaudio-sink._tcp"
#define SERVICE_NAME_SOURCE "_polypaudio-source._tcp"
#define SERVICE_NAME_SERVER "_polypaudio-server._tcp"

static const char* const valid_modargs[] = {
    "port",
    NULL
};

struct service {
    sw_discovery_oid oid;
    char *name;
    int published; /* 0 -> not yet registered, 1 -> registered with data from real device, 2 -> registered with data from autoload device */

    struct {
        int valid;
        pa_namereg_type type;
        uint32_t index;
    } loaded;

    struct {
        int valid;
        pa_namereg_type type;
        uint32_t index;
    } autoload;
};

struct userdata {
    pa_core *core;
    pa_howl_wrapper *howl_wrapper;
    pa_hashmap *services;
    pa_dynarray *sink_dynarray, *source_dynarray, *autoload_dynarray;
    pa_subscription *subscription;

    uint16_t port;
    sw_discovery_oid server_oid;
};

static sw_result publish_reply(sw_discovery discovery, sw_discovery_publish_status status, sw_discovery_oid oid, sw_opaque extra) {
    return SW_OKAY;
}

static void get_service_data(struct userdata *u, struct service *s, pa_sample_spec *ret_ss, char **ret_description, pa_typeid_t *ret_typeid) {
    assert(u && s && s->loaded.valid && ret_ss && ret_description && ret_typeid);

    if (s->loaded.type == PA_NAMEREG_SINK) {
        pa_sink *sink = pa_idxset_get_by_index(u->core->sinks, s->loaded.index);
        assert(sink);
        *ret_ss = sink->sample_spec;
        *ret_description = sink->description;
        *ret_typeid = sink->typeid;
    } else if (s->loaded.type == PA_NAMEREG_SOURCE) {
        pa_source *source = pa_idxset_get_by_index(u->core->sources, s->loaded.index);
        assert(source);
        *ret_ss = source->sample_spec;
        *ret_description = source->description;
        *ret_typeid = source->typeid;
    } else
        assert(0);
}

static void txt_record_server_data(pa_core *c, sw_text_record t) {
    char s[256];
    assert(c);

    sw_text_record_add_key_and_string_value(t, "server-version", PACKAGE_NAME" "PACKAGE_VERSION);
    sw_text_record_add_key_and_string_value(t, "user-name", pa_get_user_name(s, sizeof(s)));
    sw_text_record_add_key_and_string_value(t, "fqdn", pa_get_fqdn(s, sizeof(s)));
    snprintf(s, sizeof(s), "0x%08x", c->cookie);
    sw_text_record_add_key_and_string_value(t, "cookie", s);
}

static int publish_service(struct userdata *u, struct service *s) {
    char t[256];
    char hn[256];
    int r = -1;
    sw_text_record txt;
    int free_txt = 0;
    assert(u && s);
       
    if ((s->published == 1 && s->loaded.valid) ||
        (s->published == 2 && s->autoload.valid && !s->loaded.valid))
        return 0;

    if (s->published) {
        sw_discovery_cancel(pa_howl_wrapper_get_discovery(u->howl_wrapper), s->oid);
        s->published = 0;
    }

    snprintf(t, sizeof(t), "Networked Audio Device %s on %s", s->name, pa_get_host_name(hn, sizeof(hn)));

    if (sw_text_record_init(&txt) != SW_OKAY) {
        pa_log(__FILE__": sw_text_record_init() failed\n");
        goto finish;
    }
    free_txt = 1;

    sw_text_record_add_key_and_string_value(txt, "device", s->name);

    txt_record_server_data(u->core, txt);
    
    if (s->loaded.valid) {
        char z[64], *description;
        pa_typeid_t typeid;
        pa_sample_spec ss;

        get_service_data(u, s, &ss, &description, &typeid);
            
        snprintf(z, sizeof(z), "%u", ss.rate);
        sw_text_record_add_key_and_string_value(txt, "rate", z);
        snprintf(z, sizeof(z), "%u", ss.channels);
        sw_text_record_add_key_and_string_value(txt, "channels", z);
        sw_text_record_add_key_and_string_value(txt, "format", pa_sample_format_to_string(ss.format));

        sw_text_record_add_key_and_string_value(txt, "description", description);

        snprintf(z, sizeof(z), "0x%8x", typeid);
        sw_text_record_add_key_and_string_value(txt, "typeid", z);
        
        
        if (sw_discovery_publish(pa_howl_wrapper_get_discovery(u->howl_wrapper), 0, t,
                                 s->loaded.type == PA_NAMEREG_SINK ? SERVICE_NAME_SINK : SERVICE_NAME_SOURCE,
                                 NULL, NULL, u->port, sw_text_record_bytes(txt), sw_text_record_len(txt),
                                 publish_reply, s, &s->oid) != SW_OKAY) {
            pa_log(__FILE__": failed to register sink on zeroconf.\n");
            goto finish;
        }

        s->published = 1;
    } else if (s->autoload.valid) {

        if (sw_discovery_publish(pa_howl_wrapper_get_discovery(u->howl_wrapper), 0, t,
                                 s->autoload.type == PA_NAMEREG_SINK ? SERVICE_NAME_SINK : SERVICE_NAME_SOURCE,
                                 NULL, NULL, u->port, sw_text_record_bytes(txt), sw_text_record_len(txt),
                                 publish_reply, s, &s->oid) != SW_OKAY) {
            pa_log(__FILE__": failed to register sink on zeroconf.\n");
            goto finish;
        }

        s->published = 2;
    }

    r = 0;
    
finish:

    if (!s->published) {
        /* Remove this service */
        pa_hashmap_remove(u->services, s->name);
        pa_xfree(s->name);
        pa_xfree(s);
    }

    if (free_txt)
        sw_text_record_fina(txt);
    
    return r;
}

struct service *get_service(struct userdata *u, const char *name) {
    struct service *s;
    
    if ((s = pa_hashmap_get(u->services, name)))
        return s;
    
    s = pa_xmalloc(sizeof(struct service));
    s->published = 0;
    s->name = pa_xstrdup(name);
    s->loaded.valid = s->autoload.valid = 0;

    pa_hashmap_put(u->services, s->name, s);

    return s;
}

static int publish_sink(struct userdata *u, pa_sink *s) {
    struct service *svc;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->loaded.valid)
        return 0;

    svc->loaded.valid = 1;
    svc->loaded.type = PA_NAMEREG_SINK;
    svc->loaded.index = s->index;

    pa_dynarray_put(u->sink_dynarray, s->index, svc);

    return publish_service(u, svc);
}

static int publish_source(struct userdata *u, pa_source *s) {
    struct service *svc;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->loaded.valid)
        return 0;

    svc->loaded.valid = 1;
    svc->loaded.type = PA_NAMEREG_SOURCE;
    svc->loaded.index = s->index;

    pa_dynarray_put(u->source_dynarray, s->index, svc);
    
    return publish_service(u, svc);
}

static int publish_autoload(struct userdata *u, pa_autoload_entry *s) {
    struct service *svc;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->autoload.valid)
        return 0;

    svc->autoload.valid = 1;
    svc->autoload.type = s->type;
    svc->autoload.index = s->index;

    pa_dynarray_put(u->autoload_dynarray, s->index, svc);
    
    return publish_service(u, svc);
}

static int remove_sink(struct userdata *u, uint32_t index) {
    struct service *svc;
    assert(u && index != PA_INVALID_INDEX);

    if (!(svc = pa_dynarray_get(u->sink_dynarray, index)))
        return 0;

    if (!svc->loaded.valid || svc->loaded.type != PA_NAMEREG_SINK)
        return 0;

    svc->loaded.valid = 0;
    pa_dynarray_put(u->sink_dynarray, index, NULL);
    
    return publish_service(u, svc);
}

static int remove_source(struct userdata *u, uint32_t index) {
    struct service *svc;
    assert(u && index != PA_INVALID_INDEX);
    
    if (!(svc = pa_dynarray_get(u->source_dynarray, index)))
        return 0;

    if (!svc->loaded.valid || svc->loaded.type != PA_NAMEREG_SOURCE)
        return 0;

    svc->loaded.valid = 0;
    pa_dynarray_put(u->source_dynarray, index, NULL);

    return publish_service(u, svc);
}

static int remove_autoload(struct userdata *u, uint32_t index) {
    struct service *svc;
    assert(u && index != PA_INVALID_INDEX);
    
    if (!(svc = pa_dynarray_get(u->autoload_dynarray, index)))
        return 0;

    if (!svc->autoload.valid)
        return 0;

    svc->autoload.valid = 0;
    pa_dynarray_put(u->autoload_dynarray, index, NULL);

    return publish_service(u, svc);
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type t, uint32_t index, void *userdata) {
    struct userdata *u = userdata;
    assert(u && c);

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
        case PA_SUBSCRIPTION_EVENT_SINK: {
            if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_sink *sink;

                if ((sink = pa_idxset_get_by_index(c->sinks, index))) {
                    if (publish_sink(u, sink) < 0)
                        goto fail;
                }
            } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                if (remove_sink(u, index) < 0)
                    goto fail;
            }
        
            break;

        case PA_SUBSCRIPTION_EVENT_SOURCE:

            if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_source *source;
                
                if ((source = pa_idxset_get_by_index(c->sources, index))) {
                    if (publish_source(u, source) < 0)
                        goto fail;
                }
            } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                if (remove_source(u, index) < 0)
                    goto fail;
            }
            
            break;

        case PA_SUBSCRIPTION_EVENT_AUTOLOAD:
            if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_autoload_entry *autoload;
                    
                if ((autoload = pa_idxset_get_by_index(c->autoload_idxset, index))) {
                    if (publish_autoload(u, autoload) < 0)
                        goto fail;
                }
            } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                if (remove_autoload(u, index) < 0)
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

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u;
    uint32_t index, port = PA_NATIVE_DEFAULT_PORT;
    pa_sink *sink;
    pa_source *source;
    pa_autoload_entry *autoload;
    pa_modargs *ma = NULL;
    char t[256], hn[256];
    int free_txt = 0;
    sw_text_record txt;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.\n");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port == 0 || port >= 0xFFFF) {
        pa_log(__FILE__": invalid port specified.\n");
        goto fail;
    }

    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->port = (uint16_t) port;

    if (!(u->howl_wrapper = pa_howl_wrapper_get(c)))
        goto fail;

    u->services = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->sink_dynarray = pa_dynarray_new();
    u->source_dynarray = pa_dynarray_new();
    u->autoload_dynarray = pa_dynarray_new();
    
    u->subscription = pa_subscription_new(c,
                                          PA_SUBSCRIPTION_MASK_SINK|
                                          PA_SUBSCRIPTION_MASK_SOURCE|
                                          PA_SUBSCRIPTION_MASK_AUTOLOAD, subscribe_callback, u);

    for (sink = pa_idxset_first(c->sinks, &index); sink; sink = pa_idxset_next(c->sinks, &index))
        if (publish_sink(u, sink) < 0)
            goto fail;

    for (source = pa_idxset_first(c->sources, &index); source; source = pa_idxset_next(c->sources, &index))
        if (publish_source(u, source) < 0)
            goto fail;

    if (c->autoload_idxset)
        for (autoload = pa_idxset_first(c->autoload_idxset, &index); autoload; autoload = pa_idxset_next(c->autoload_idxset, &index))
            if (publish_autoload(u, autoload) < 0)
                goto fail;

    snprintf(t, sizeof(t), "Networked Audio Server on %s", pa_get_host_name(hn, sizeof(hn)));   

    if (sw_text_record_init(&txt) != SW_OKAY) {
        pa_log(__FILE__": sw_text_record_init() failed\n");
        goto fail;
    }
    free_txt = 1;

    txt_record_server_data(u->core, txt);
    
    if (sw_discovery_publish(pa_howl_wrapper_get_discovery(u->howl_wrapper), 0, t,
                             SERVICE_NAME_SERVER,
                             NULL, NULL, u->port, sw_text_record_bytes(txt), sw_text_record_len(txt),
                             publish_reply, u, &u->server_oid) != SW_OKAY) {
        pa_log(__FILE__": failed to register server on zeroconf.\n");
        goto fail;
    }
    
    sw_text_record_fina(txt);
    pa_modargs_free(ma);
    
    return 0;
    
fail:
    pa__done(c, m);

    if (ma)
        pa_modargs_free(ma);

    if (free_txt)
        sw_text_record_fina(txt);
    
    return -1;
}

static void service_free(void *p, void *userdata) {
    struct service *s = p;
    struct userdata *u = userdata;
    assert(s && u);
    sw_discovery_cancel(pa_howl_wrapper_get_discovery(u->howl_wrapper), s->oid);
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

    if (u->sink_dynarray)
        pa_dynarray_free(u->sink_dynarray, NULL, NULL);
    if (u->source_dynarray)
        pa_dynarray_free(u->source_dynarray, NULL, NULL);
    if (u->autoload_dynarray)
        pa_dynarray_free(u->autoload_dynarray, NULL, NULL);
    
    if (u->subscription)
        pa_subscription_free(u->subscription);
    
    if (u->howl_wrapper)
        pa_howl_wrapper_unref(u->howl_wrapper);

    
    pa_xfree(u);
}

