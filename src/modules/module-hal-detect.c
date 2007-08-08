/* $Id$ */

/***
    This file is part of PulseAudio.

    Copyright 2006 Lennart Poettering
    Copyright 2006 Shams E. King

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2 of the License,
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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/core-util.h>

#include <hal/libhal.h>

#include "dbus-util.h"
#include "module-hal-detect-symdef.h"

PA_MODULE_AUTHOR("Shahms King")
PA_MODULE_DESCRIPTION("Detect available audio hardware and load matching drivers")
PA_MODULE_VERSION(PACKAGE_VERSION)

struct device {
    uint32_t index;
    char *udi;
};

struct userdata {
    pa_core *core;
    LibHalContext *context;
    pa_dbus_connection *connection;
    pa_hashmap *devices;
    const char *capability;
};

struct timerdata {
    struct userdata *u;
    char *udi;
};

#define CAPABILITY_ALSA "alsa"
#define CAPABILITY_OSS "oss"

static void hal_device_free(struct device* d) {
    pa_assert(d);
    
    pa_xfree(d->udi);
    pa_xfree(d);
}

static void hal_device_free_cb(void *d, PA_GCC_UNUSED void *data) {
    hal_device_free(d);
}

static const char *strip_udi(const char *udi) {
    const char *slash;
    
    if ((slash = strrchr(udi, '/')))
        return slash+1;

    return udi;
}

#ifdef HAVE_ALSA

typedef enum {
    ALSA_TYPE_SINK,
    ALSA_TYPE_SOURCE,
    ALSA_TYPE_OTHER,
    ALSA_TYPE_MAX
} alsa_type_t;

static alsa_type_t hal_alsa_device_get_type(LibHalContext *context, const char *udi, DBusError *error) {
    char *type;
    alsa_type_t t;

    if (!(type = libhal_device_get_property_string(context, udi, "alsa.type", error)))
        return ALSA_TYPE_OTHER;

    if (!strcmp(type, "playback")) 
        t = ALSA_TYPE_SINK;
    else if (!strcmp(type, "capture"))
        t = ALSA_TYPE_SOURCE;
    else 
        t = ALSA_TYPE_OTHER;

    libhal_free_string(type);

    return t;
}

static int hal_alsa_device_is_modem(LibHalContext *context, const char *udi, DBusError *error) {
    char *class;
    int r;
    
    if (!(class = libhal_device_get_property_string(context, udi, "alsa.pcm_class", error)))
        return 0;

    r = strcmp(class, "modem") == 0;
    pa_xfree(class);
    
    return r;
}

static pa_module* hal_device_load_alsa(struct userdata *u, const char *udi) {
    char args[128];
    alsa_type_t type;
    int device, card;
    const char *module_name;
    DBusError error;
    
    dbus_error_init(&error);

    type = hal_alsa_device_get_type(u->context, udi, &error);
    if (dbus_error_is_set(&error) || type == ALSA_TYPE_OTHER)
        goto fail;

    device = libhal_device_get_property_int(u->context, udi, "alsa.device", &error);
    if (dbus_error_is_set(&error) || device != 0)
        goto fail;

    card = libhal_device_get_property_int(u->context, udi, "alsa.card", &error);
    if (dbus_error_is_set(&error))
        goto fail;

    if (hal_alsa_device_is_modem(u->context, udi, &error))
        goto fail;

    if (type == ALSA_TYPE_SINK) {
        module_name = "module-alsa-sink";
        pa_snprintf(args, sizeof(args), "device=hw:%u sink_name=alsa_output.%s", card, strip_udi(udi));
    } else {
        module_name = "module-alsa-source";
        pa_snprintf(args, sizeof(args), "device=hw:%u source_name=alsa_input.%s", card, strip_udi(udi));
    }

    pa_log_debug("Loading %s with arguments '%s'", module_name, args);

    return pa_module_load(u->core, module_name, args);

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing ALSA data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return NULL;
}

#endif

#ifdef HAVE_OSS

static int hal_oss_device_is_pcm(LibHalContext *context, const char *udi, DBusError *error) {
    char *class = NULL, *dev = NULL, *e;
    int device;
    int r = 0;

    class = libhal_device_get_property_string(context, udi, "oss.type", error);
    if (dbus_error_is_set(error) || !class)
        goto finish;

    if (strcmp(class, "pcm"))
        goto finish;

    dev = libhal_device_get_property_string(context, udi, "oss.device_file", error);
    if (dbus_error_is_set(error) || !dev)
        goto finish;

    if ((e = strrchr(dev, '/')))
        if (pa_startswith(e + 1, "audio"))
            goto finish;

    device = libhal_device_get_property_int(context, udi, "oss.device", error);
    if (dbus_error_is_set(error) || device != 0)
        goto finish;

    r = 1;

finish:

    libhal_free_string(class);
    libhal_free_string(dev);
    
    return r;
}

static pa_module* hal_device_load_oss(struct userdata *u, const char *udi) {
    char args[256];
    char* device;
    DBusError error;
    
    dbus_error_init(&error);

    if (!hal_oss_device_is_pcm(u->context, udi, &error) || dbus_error_is_set(&error))
        goto fail;

    device = libhal_device_get_property_string(u->context, udi, "oss.device_file", &error);
    if (!device || dbus_error_is_set(&error))
        goto fail;

    pa_snprintf(args, sizeof(args), "device=%s sink_name=oss_output.%s source_name=oss_input.%s", device, strip_udi(udi), strip_udi(udi));
    libhal_free_string(device);

    pa_log_debug("Loading module-oss with arguments '%s'", args);

    return pa_module_load(u->core, "module-oss", args);

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing OSS data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return NULL;
}
#endif

static int hal_device_add(struct userdata *u, const char *udi) {
    pa_module* m = NULL;
    struct device *d;

    pa_assert(u);
    pa_assert(u->capability);

#ifdef HAVE_ALSA
    if (strcmp(u->capability, CAPABILITY_ALSA) == 0)
        m = hal_device_load_alsa(u, udi);
#endif
#ifdef HAVE_OSS
    if (strcmp(u->capability, CAPABILITY_OSS) == 0)
        m = hal_device_load_oss(u, udi);
#endif

    if (!m)
        return -1;

    d = pa_xnew(struct device, 1);
    d->udi = pa_xstrdup(udi);
    d->index = m->index;
    pa_hashmap_put(u->devices, d->udi, d);

    return 0;
}

static int hal_device_add_all(struct userdata *u, const char *capability) {
    DBusError error;
    int i, n, count = 0;
    char** udis;

    pa_assert(u);
    pa_assert(!u->capability);
    
    dbus_error_init(&error);

    pa_log_info("Trying capability %s", capability);

    udis = libhal_find_device_by_capability(u->context, capability, &n, &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error("Error finding devices: %s: %s", error.name, error.message);
        dbus_error_free(&error);
        return -1;
    }

    if (n > 0) {
        u->capability = capability;
        
        for (i = 0; i < n; i++) {
            if (hal_device_add(u, udis[i]) < 0)
                pa_log_debug("Not loaded device %s", udis[i]);
            else
                count++;
        }
    }

    libhal_free_string_array(udis);
    return count;
}

static dbus_bool_t device_has_capability(LibHalContext *context, const char *udi, const char* cap, DBusError *error){
    dbus_bool_t has_prop;
    
    has_prop = libhal_device_property_exists(context, udi, "info.capabilities", error);
    if (!has_prop || dbus_error_is_set(error))
        return FALSE;

    return libhal_device_query_capability(context, udi, cap, error);
}

static void device_added_time_cb(pa_mainloop_api *ea, pa_time_event *ev, const struct timeval *tv, void *userdata) {
    DBusError error;
    struct timerdata *td = userdata;
    int b;

    dbus_error_init(&error);
    
    b = libhal_device_exists(td->u->context, td->udi, &error);
    
    if (dbus_error_is_set(&error)) {
        pa_log_error("Error adding device: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    } else if (b)
        hal_device_add(td->u, td->udi);

    pa_xfree(td->udi);
    pa_xfree(td);
    ea->time_free(ev);
}

static void device_added_cb(LibHalContext *context, const char *udi) {
    DBusError error;
    struct timeval tv;
    struct timerdata *t;
    struct userdata *u;
    int good = 0;

    pa_assert_se(u = libhal_ctx_get_user_data(context));
    
    pa_log_debug("HAL Device added: %s", udi);

    dbus_error_init(&error);

    if (u->capability) {
        
        good = device_has_capability(context, udi, u->capability, &error);

        if (dbus_error_is_set(&error)) {
            pa_log_error("Error getting capability: %s: %s", error.name, error.message);
            dbus_error_free(&error);
            return;
        }
        
    } else {

#ifdef HAVE_ALSA
        good = device_has_capability(context, udi, CAPABILITY_ALSA, &error);

        if (dbus_error_is_set(&error)) {
            pa_log_error("Error getting capability: %s: %s", error.name, error.message);
            dbus_error_free(&error);
            return;
        }

        if (good)
            u->capability = CAPABILITY_ALSA;
#endif
#if defined(HAVE_OSS) && defined(HAVE_ALSA)
        if (!good) {
#endif        
#ifdef HAS_OSS
            good = device_has_capability(context, udi, CAPABILITY_OSS, &error);
            
            if (dbus_error_is_set(&error)) {
                pa_log_error("Error getting capability: %s: %s", error.name, error.message);
                dbus_error_free(&error);
                return;
            }

            if (good)
                u->capability = CAPABILITY_OSS;

#endif
#if defined(HAVE_OSS) && defined(HAVE_ALSA)
        }
#endif
    }
        
    if (!good)
        return;

    /* actually add the device 1/2 second later */
    t = pa_xnew(struct timerdata, 1);
    t->u = u;
    t->udi = pa_xstrdup(udi);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, 500000);
    u->core->mainloop->time_new(u->core->mainloop, &tv, device_added_time_cb, t);
}

static void device_removed_cb(LibHalContext* context, const char *udi) {
    struct device *d;
    struct userdata *u;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    pa_log_debug("Device removed: %s", udi);
    
    if ((d = pa_hashmap_remove(u->devices, udi))) {
        pa_module_unload_by_index(u->core, d->index);
        hal_device_free(d);
    }
}

static void new_capability_cb(LibHalContext *context, const char *udi, const char* capability) {
    struct userdata *u;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (!u->capability || strcmp(u->capability, capability) == 0)
        /* capability we care about, pretend it's a new device */
        device_added_cb(context, udi);
}

static void lost_capability_cb(LibHalContext *context, const char *udi, const char* capability) {
    struct userdata *u;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (u->capability && strcmp(u->capability, capability) == 0)
        /* capability we care about, pretend it was removed */
        device_removed_cb(context, udi);
}

static void hal_context_free(LibHalContext* hal_context) {
    DBusError error;

    dbus_error_init(&error);
    
    libhal_ctx_shutdown(hal_context, &error);
    libhal_ctx_free(hal_context);

    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
}

static LibHalContext* hal_context_new(pa_core* c, DBusConnection *conn) {
    DBusError error;
    LibHalContext *hal_context = NULL;

    dbus_error_init(&error);
    
    if (!(hal_context = libhal_ctx_new())) {
        pa_log_error("libhal_ctx_new() failed");
        goto fail;
    }

    if (!libhal_ctx_set_dbus_connection(hal_context, conn)) {
        pa_log_error("Error establishing DBUS connection: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!libhal_ctx_init(hal_context, &error)) {
        pa_log_error("Couldn't connect to hald: %s: %s", error.name, error.message);
        goto fail;
    }

    return hal_context;

fail:
    if (hal_context)
        hal_context_free(hal_context);

    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    return NULL;
}

int pa__init(pa_core *c, pa_module*m) {
    DBusError error;
    pa_dbus_connection *conn;
    struct userdata *u = NULL;
    LibHalContext *hal_context = NULL;
    int n = 0;
    
    pa_assert(c);
    pa_assert(m);

    dbus_error_init(&error);
    
    if (!(conn = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &error))) {
        pa_log_error("Unable to contact DBUS system bus: %s: %s", error.name, error.message);
        dbus_error_free(&error);
        return -1;
    }

    if (!(hal_context = hal_context_new(c, pa_dbus_connection_get(conn)))) {
        /* pa_hal_context_new() logs appropriate errors */
        pa_dbus_connection_unref(conn);
        return -1;
    }

    u = pa_xnew(struct userdata, 1);
    u->core = c;
    u->context = hal_context;
    u->connection = conn;
    u->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->capability = NULL;
    m->userdata = u;

#ifdef HAVE_ALSA
    n = hal_device_add_all(u, CAPABILITY_ALSA);
#endif
#if defined(HAVE_ALSA) && defined(HAVE_OSS)
    if (n <= 0)
#endif        
#ifdef HAVE_OSS
        n += hal_device_add_all(u, CAPABILITY_OSS);
#endif

    libhal_ctx_set_user_data(hal_context, u);
    libhal_ctx_set_device_added(hal_context, device_added_cb);
    libhal_ctx_set_device_removed(hal_context, device_removed_cb);
    libhal_ctx_set_device_new_capability(hal_context, new_capability_cb);
    libhal_ctx_set_device_lost_capability(hal_context, lost_capability_cb);

    dbus_error_init(&error);
    
    if (!libhal_device_property_watch_all(hal_context, &error)) {
        pa_log_error("Error monitoring device list: %s: %s", error.name, error.message);
        dbus_error_free(&error);
        pa__done(c, m);
        return -1;
    }

    pa_log_info("Loaded %i modules.", n);

    return 0;
}


void pa__done(PA_GCC_UNUSED pa_core *c, pa_module *m) {
    struct userdata *u;
    
    pa_assert(c);
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->context)
        hal_context_free(u->context);

    if (u->devices)
        pa_hashmap_free(u->devices, hal_device_free_cb, NULL);

    if (u->connection)
        pa_dbus_connection_unref(u->connection);
    
    pa_xfree(u);
}
