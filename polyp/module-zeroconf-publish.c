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

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("mDNS/DNS-SD Service Publisher")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define SERVICE_NAME_SINK "_polypaudio-sink._tcp"
#define SERVICE_NAME_SOURCE "_polypaudio-source._tcp"
#define SERVICE_NAME_SERVER "_polypaudio-server._tcp"

struct service {
    sw_discovery_oid oid;
    char *name;
    int published; /* 0 -> not yet registered, 1 -> registered with data from real device, 2 -> registered with data from autoload device */

    struct {
        int valid;
        enum pa_namereg_type type;
        uint32_t index;
    } loaded;

    struct {
        int valid;
        enum pa_namereg_type type;
        uint32_t index;
    } autoload;
};

struct userdata {
    struct pa_core *core;
    struct pa_howl_wrapper *howl_wrapper;
    struct pa_hashmap *services;
    struct pa_subscription *subscription;
};

static sw_result publish_reply(sw_discovery discovery, sw_discovery_publish_status status, sw_discovery_oid oid, sw_opaque extra) {
    return SW_OKAY;
}

static void get_service_sample_spec(struct userdata *u, struct service *s, struct pa_sample_spec *ret_ss) {
    assert(u && s && s->loaded.valid && ret_ss);

    if (s->loaded.type == PA_NAMEREG_SINK) {
        struct pa_sink *sink = pa_idxset_get_by_index(u->core->sinks, s->loaded.index);
        assert(sink);
        *ret_ss = sink->sample_spec;
    } else if (s->loaded.type == PA_NAMEREG_SOURCE) {
        struct pa_source *source = pa_idxset_get_by_index(u->core->sources, s->loaded.index);
        assert(source);
        *ret_ss = source->sample_spec;
    } else
        assert(0);
}

static int publish_service(struct userdata *u, struct service *s) {
    assert(u && s);
    char t[256];
    char hn[256];
    int r = -1;
    sw_text_record txt;
    int free_txt = 0;
       
    if ((s->published == 1 && s->loaded.valid) ||
        (s->published == 2 && s->autoload.valid && !s->loaded.valid))
        return 0;

    if (s->published) {
        sw_discovery_cancel(pa_howl_wrapper_get_discovery(u->howl_wrapper), s->oid);
        s->published = 0;
    }

    snprintf(t, sizeof(t), "%s@%s", s->name, pa_get_host_name(hn, sizeof(hn)));   

    if (sw_text_record_init(&txt) != SW_OKAY) {
        pa_log(__FILE__": sw_text_record_init() failed\n");
        goto finish;
    }
    free_txt = 1;

    sw_text_record_add_key_and_string_value(txt, "device", s->name);
    
    if (s->loaded.valid) {
        char z[64];
        struct pa_sample_spec ss;

        get_service_sample_spec(u, s, &ss);
            
        snprintf(z, sizeof(z), "%u", ss.rate);
        sw_text_record_add_key_and_string_value(txt, "rate", z);
        snprintf(z, sizeof(z), "%u", ss.channels);
        sw_text_record_add_key_and_string_value(txt, "channels", z);
        sw_text_record_add_key_and_string_value(txt, "format", pa_sample_format_to_string(ss.format));
        
        if (sw_discovery_publish(pa_howl_wrapper_get_discovery(u->howl_wrapper), 0, t,
                                 s->loaded.type == PA_NAMEREG_SINK ? SERVICE_NAME_SINK : SERVICE_NAME_SOURCE,
                                 NULL, NULL, PA_NATIVE_DEFAULT_PORT, sw_text_record_bytes(txt), sw_text_record_len(txt),
                                 publish_reply, s, &s->oid) != SW_OKAY) {
            pa_log(__FILE__": failed to register sink on zeroconf.\n");
            goto finish;
        }

        s->published = 1;
    } else if (s->autoload.valid) {

        if (sw_discovery_publish(pa_howl_wrapper_get_discovery(u->howl_wrapper), 0, t,
                                 s->autoload.type == PA_NAMEREG_SINK ? SERVICE_NAME_SINK : SERVICE_NAME_SOURCE,
                                 NULL, NULL, PA_NATIVE_DEFAULT_PORT, sw_text_record_bytes(txt), sw_text_record_len(txt),
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

static int publish_sink(struct userdata *u, struct pa_sink *s) {
    struct service *svc;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->loaded.valid)
        return 0;

    svc->loaded.valid = 1;
    svc->loaded.type = PA_NAMEREG_SINK;
    svc->loaded.index = s->index;

    return publish_service(u, svc);
}

static int publish_source(struct userdata *u, struct pa_source *s) {
    struct service *svc;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->loaded.valid)
        return 0;

    svc->loaded.valid = 1;
    svc->loaded.type = PA_NAMEREG_SOURCE;
    svc->loaded.index = s->index;
    
    return publish_service(u, svc);
}

static int publish_autoload(struct userdata *u, struct pa_autoload_entry *s) {
    struct service *svc;
    assert(u && s);

    svc = get_service(u, s->name);
    if (svc->autoload.valid)
        return 0;

    svc->autoload.valid = 1;
    svc->autoload.type = s->type;
    svc->autoload.index = s->index;
    
    return publish_service(u, svc);
}

static int remove_sink(struct userdata *u, struct pa_sink *s) {
    struct service *svc;
    assert(u && s);

    if (!(svc = pa_hashmap_get(u->services, s->name)))
        return 0;

    if (!svc->loaded.valid || svc->loaded.type != PA_NAMEREG_SINK)
        return 0;

    svc->loaded.valid = 0;
    return publish_service(u, svc);
}

static int remove_source(struct userdata *u, struct pa_source *s) {
    struct service *svc;
    assert(u && s);
    
    if (!(svc = pa_hashmap_get(u->services, s->name)))
        return 0;

    if (!svc->loaded.valid || svc->loaded.type != PA_NAMEREG_SOURCE)
        return 0;

    svc->loaded.valid = 0;
    return publish_service(u, svc);
}

static int remove_autoload(struct userdata *u, struct pa_autoload_entry *s) {
    struct service *svc;
    assert(u && s);
    
    if (!(svc = pa_hashmap_get(u->services, s->name)))
        return 0;

    if (!svc->autoload.valid || svc->autoload.type != s->type)
        return 0;

    svc->autoload.valid = 0;
    return publish_service(u, svc);
}

static void subscribe_callback(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata) {
    struct userdata *u = userdata;
    assert(u && c);

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
        case PA_SUBSCRIPTION_EVENT_SINK: {
            struct pa_sink *sink;

            pa_log("subscribe: %x\n", t);
    
    

            
            if ((sink = pa_idxset_get_by_index(c->sinks, index))) {
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                    pa_log("add\n");
                    if (publish_sink(u, sink) < 0)
                        goto fail;
                } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                    pa_log("remove\n");


                    if (remove_sink(u, sink) < 0)
                        goto fail;
                }
            }
        
            break;
        }

        case PA_SUBSCRIPTION_EVENT_SOURCE: {
            struct pa_source *source;

            if ((source = pa_idxset_get_by_index(c->sources, index))) {
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                    if (publish_source(u, source) < 0)
                        goto fail;
                } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                    if (remove_source(u, source) < 0)
                        goto fail;
                }
            }
            
            break;
        }

        case PA_SUBSCRIPTION_EVENT_AUTOLOAD: {
            struct pa_autoload_entry *autoload;
            
            if ((autoload = pa_idxset_get_by_index(c->autoload_idxset, index))) {
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
                    if (publish_autoload(u, autoload) < 0)
                        goto fail;
                } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                    if (remove_autoload(u, autoload) < 0)
                        goto fail;
                }
            }
            
            break;
        }
    }

    return;

fail:
    if (u->subscription) {
        pa_subscription_free(u->subscription);
        u->subscription = NULL;
    }
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    uint32_t index;
    struct pa_sink *sink;
    struct pa_source *source;
    struct pa_autoload_entry *autoload;

    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;

    if (!(u->howl_wrapper = pa_howl_wrapper_get(c)))
        goto fail;

    u->services = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

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

    return 0;
    
fail:
    pa__done(c, m);
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

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata*u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    if (u->services)
        pa_hashmap_free(u->services, service_free, u);

    if (u->subscription)
        pa_subscription_free(u->subscription);
    
    if (u->howl_wrapper)
        pa_howl_wrapper_unref(u->howl_wrapper);
    
    pa_xfree(u);
}

