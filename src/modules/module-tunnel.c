/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include <polypcore/module.h>
#include <polypcore/util.h>
#include <polypcore/modargs.h>
#include <polypcore/log.h>
#include <polypcore/core-subscribe.h>
#include <polypcore/xmalloc.h>
#include <polypcore/sink-input.h>
#include <polypcore/pdispatch.h>
#include <polypcore/pstream.h>
#include <polypcore/pstream-util.h>
#include <polypcore/authkey.h>
#include <polypcore/socket-client.h>
#include <polypcore/socket-util.h>
#include <polypcore/authkey-prop.h>

#ifdef TUNNEL_SINK
#include "module-tunnel-sink-symdef.h"
PA_MODULE_DESCRIPTION("Tunnel module for sinks")
PA_MODULE_USAGE("server=<address> sink=<remote sink name> cookie=<filename> format=<sample format> channels=<number of channels> rate=<sample rate> sink_name=<name for the local sink>")
#else
#include "module-tunnel-source-symdef.h"
PA_MODULE_DESCRIPTION("Tunnel module for sources")
PA_MODULE_USAGE("server=<address> source=<remote source name> cookie=<filename> format=<sample format> channels=<number of channels> rate=<sample rate> source_name=<name for the local source>")
#endif

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define DEFAULT_SINK_NAME "tunnel"
#define DEFAULT_SOURCE_NAME "tunnel"

#define DEFAULT_TLENGTH (44100*2*2/10)  //(10240*8)
#define DEFAULT_MAXLENGTH ((DEFAULT_TLENGTH*3)/2)
#define DEFAULT_MINREQ 512
#define DEFAULT_PREBUF (DEFAULT_TLENGTH-DEFAULT_MINREQ)
#define DEFAULT_FRAGSIZE 1024

#define DEFAULT_TIMEOUT 5

#define LATENCY_INTERVAL 10

static const char* const valid_modargs[] = {
    "server",
    "cookie",
    "format",
    "channels",
    "rate",
#ifdef TUNNEL_SINK
    "sink_name",
    "sink",
#else
    "source_name",
    "source",
#endif
    NULL,
};

static void command_stream_killed(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

#ifdef TUNNEL_SINK
static void command_request(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
#endif

static const pa_pdispatch_callback_t command_table[PA_COMMAND_MAX] = {
#ifdef TUNNEL_SINK
    [PA_COMMAND_REQUEST] = command_request,
#endif    
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = command_stream_killed,
    [PA_COMMAND_RECORD_STREAM_KILLED] = command_stream_killed
};

struct userdata {
    pa_socket_client *client;
    pa_pstream *pstream;
    pa_pdispatch *pdispatch;

    char *server_name;
#ifdef TUNNEL_SINK
    char *sink_name;
    pa_sink *sink;
    uint32_t requested_bytes;
#else
    char *source_name;
    pa_source *source;
#endif
    
    pa_module *module;
    pa_core *core;

    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];

    uint32_t ctag;
    uint32_t device_index;
    uint32_t channel;
    
    pa_usec_t host_latency;

    pa_time_event *time_event;

    int auth_cookie_in_property;
};

static void close_stuff(struct userdata *u) {
    assert(u);
    
    if (u->pstream) {
        pa_pstream_close(u->pstream);
        pa_pstream_unref(u->pstream);
        u->pstream = NULL;
    }

    if (u->pdispatch) {
        pa_pdispatch_unref(u->pdispatch);
        u->pdispatch = NULL;
    }

    if (u->client) {
        pa_socket_client_unref(u->client);
        u->client = NULL;
    }

#ifdef TUNNEL_SINK
    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }
#else
    if (u->source) {
        pa_source_disconnect(u->source);
        pa_source_unref(u->source);
        u->source = NULL;
    }
#endif

    if (u->time_event) {
        u->core->mainloop->time_free(u->time_event);
        u->time_event = NULL;
    }
}

static void die(struct userdata *u) {
    assert(u);
    close_stuff(u);
    pa_module_unload_request(u->module);
}

static void command_stream_killed(pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    assert(pd && t && u && u->pdispatch == pd);

    pa_log(__FILE__": stream killed\n");
    die(u);
}

#ifdef TUNNEL_SINK
static void send_prebuf_request(struct userdata *u) {
    pa_tagstruct *t;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_PREBUF_PLAYBACK_STREAM);
    pa_tagstruct_putu32(t, u->ctag++);
    pa_tagstruct_putu32(t, u->channel);
    pa_pstream_send_tagstruct(u->pstream, t);
}

static void send_bytes(struct userdata *u) {
    assert(u);

    if (!u->pstream)
        return;

    while (u->requested_bytes > 0) {
        pa_memchunk chunk;
        if (pa_sink_render(u->sink, u->requested_bytes, &chunk) < 0) {

            
            if (u->requested_bytes >= DEFAULT_TLENGTH-DEFAULT_PREBUF) 
                send_prebuf_request(u);
            
            return;
        }

        pa_pstream_send_memblock(u->pstream, u->channel, 0, PA_SEEK_RELATIVE, &chunk);
        pa_memblock_unref(chunk.memblock);

        if (chunk.length > u->requested_bytes)
            u->requested_bytes = 0;
        else
            u->requested_bytes -= chunk.length;
    }
}

static void command_request(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    uint32_t bytes, channel;
    assert(pd && command == PA_COMMAND_REQUEST && t && u && u->pdispatch == pd);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_log(__FILE__": invalid protocol reply\n");
        die(u);
        return;
    }

    if (channel != u->channel) {
        pa_log(__FILE__": recieved data for invalid channel\n");
        die(u);
        return;
    }
    
    u->requested_bytes += bytes;
    send_bytes(u);
}

#endif

static void stream_get_latency_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    pa_usec_t buffer_usec, sink_usec, source_usec, transport_usec;
    int playing;
    uint32_t queue_length;
    uint64_t counter;
    struct timeval local, remote, now;
    assert(pd && u);

    if (command != PA_COMMAND_REPLY) {
        if (command == PA_COMMAND_ERROR)
            pa_log(__FILE__": failed to get latency.\n");
        else
            pa_log(__FILE__": protocol error.\n");
        die(u);
        return;
    }
    
    if (pa_tagstruct_get_usec(t, &buffer_usec) < 0 ||
        pa_tagstruct_get_usec(t, &sink_usec) < 0 ||
        pa_tagstruct_get_usec(t, &source_usec) < 0 ||
        pa_tagstruct_get_boolean(t, &playing) < 0 ||
        pa_tagstruct_getu32(t, &queue_length) < 0 ||
        pa_tagstruct_get_timeval(t, &local) < 0 ||
        pa_tagstruct_get_timeval(t, &remote) < 0 ||
        pa_tagstruct_getu64(t, &counter) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_log(__FILE__": invalid reply.\n");
        die(u);
        return;
    }

    pa_gettimeofday(&now);

    if (pa_timeval_cmp(&local, &remote) < 0 && pa_timeval_cmp(&remote, &now)) {
        /* local and remote seem to have synchronized clocks */
#ifdef TUNNEL_SINK
        transport_usec = pa_timeval_diff(&remote, &local);
#else
        transport_usec = pa_timeval_diff(&now, &remote);
#endif    
    } else
        transport_usec = pa_timeval_diff(&now, &local)/2;

#ifdef TUNNEL_SINK
    u->host_latency = sink_usec + transport_usec;
#else
    u->host_latency = source_usec + transport_usec;
    if (u->host_latency > sink_usec)
        u->host_latency -= sink_usec;
    else
        u->host_latency = 0;
#endif

/*     pa_log(__FILE__": estimated host latency: %0.0f usec\n", (double) u->host_latency); */
}

static void request_latency(struct userdata *u) {
    pa_tagstruct *t;
    struct timeval now;
    uint32_t tag;
    assert(u);

    t = pa_tagstruct_new(NULL, 0);
#ifdef TUNNEL_SINK    
    pa_tagstruct_putu32(t, PA_COMMAND_GET_PLAYBACK_LATENCY);
#else
    pa_tagstruct_putu32(t, PA_COMMAND_GET_RECORD_LATENCY);
#endif
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, u->channel);

    pa_gettimeofday(&now);
    pa_tagstruct_put_timeval(t, &now);
    pa_tagstruct_putu64(t, 0);
    
    pa_pstream_send_tagstruct(u->pstream, t);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_latency_callback, u);
}

static void create_stream_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    assert(pd && u && u->pdispatch == pd);

    if (command != PA_COMMAND_REPLY) {
        if (command == PA_COMMAND_ERROR)
            pa_log(__FILE__": failed to create stream.\n");
        else
            pa_log(__FILE__": protocol error.\n");
        die(u);
        return;
    }

    if (pa_tagstruct_getu32(t, &u->channel) < 0 ||
        pa_tagstruct_getu32(t, &u->device_index) < 0 ||
#ifdef TUNNEL_SINK        
        pa_tagstruct_getu32(t, &u->requested_bytes) < 0 ||
#endif        
        !pa_tagstruct_eof(t)) {
        pa_log(__FILE__": invalid reply.\n");
        die(u);
        return;
    }

    request_latency(u);
#ifdef TUNNEL_SINK
    send_bytes(u);
#endif
}

static void setup_complete_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    pa_tagstruct *reply;
    char name[256], un[128], hn[128];
    assert(pd && u && u->pdispatch == pd);

    if (command != PA_COMMAND_REPLY || !pa_tagstruct_eof(t)) {
        if (command == PA_COMMAND_ERROR)
            pa_log(__FILE__": failed to authenticate\n");
        else
            pa_log(__FILE__": protocol error.\n");
        die(u);
        return;
    }
#ifdef TUNNEL_SINK
    snprintf(name, sizeof(name), "Tunnel from host '%s', user '%s', sink '%s'",
             pa_get_host_name(hn, sizeof(hn)),
             pa_get_user_name(un, sizeof(un)),
             u->sink->name);
#else
    snprintf(name, sizeof(name), "Tunnel from host '%s', user '%s', source '%s'",
             pa_get_host_name(hn, sizeof(hn)),
             pa_get_user_name(un, sizeof(un)),
             u->source->name);
#endif
    
    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_SET_CLIENT_NAME);
    pa_tagstruct_putu32(reply, tag = u->ctag++);
    pa_tagstruct_puts(reply, name);
    pa_pstream_send_tagstruct(u->pstream, reply);
    /* We ignore the server's reply here */

    reply = pa_tagstruct_new(NULL, 0);
#ifdef TUNNEL_SINK    
    pa_tagstruct_putu32(reply, PA_COMMAND_CREATE_PLAYBACK_STREAM);
    pa_tagstruct_putu32(reply, tag = u->ctag++);
    pa_tagstruct_puts(reply, name);
    pa_tagstruct_put_sample_spec(reply, &u->sink->sample_spec);
    pa_tagstruct_putu32(reply, PA_INVALID_INDEX);
    pa_tagstruct_puts(reply, u->sink_name);
    pa_tagstruct_putu32(reply, DEFAULT_MAXLENGTH);
    pa_tagstruct_put_boolean(reply, 0);
    pa_tagstruct_putu32(reply, DEFAULT_TLENGTH);
    pa_tagstruct_putu32(reply, DEFAULT_PREBUF);
    pa_tagstruct_putu32(reply, DEFAULT_MINREQ);
    pa_tagstruct_putu32(reply, PA_VOLUME_NORM);
#else
    pa_tagstruct_putu32(reply, PA_COMMAND_CREATE_RECORD_STREAM);
    pa_tagstruct_putu32(reply, tag = u->ctag++);
    pa_tagstruct_puts(reply, name);
    pa_tagstruct_put_sample_spec(reply, &u->source->sample_spec);
    pa_tagstruct_putu32(reply, PA_INVALID_INDEX);
    pa_tagstruct_puts(reply, u->source_name);
    pa_tagstruct_putu32(reply, DEFAULT_MAXLENGTH);
    pa_tagstruct_put_boolean(reply, 0);
    pa_tagstruct_putu32(reply, DEFAULT_FRAGSIZE);
#endif
    
    pa_pstream_send_tagstruct(u->pstream, reply);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, create_stream_callback, u);
}

static void pstream_die_callback(pa_pstream *p, void *userdata) {
    struct userdata *u = userdata;
    assert(p && u);

    pa_log(__FILE__": stream died.\n");
    die(u);
}


static void pstream_packet_callback(pa_pstream *p, pa_packet *packet, void *userdata) {
    struct userdata *u = userdata;
    assert(p && packet && u);

    if (pa_pdispatch_run(u->pdispatch, packet, u) < 0) {
        pa_log(__FILE__": invalid packet\n");
        die(u);
    }
}

#ifndef TUNNEL_SINK
static void pstream_memblock_callback(pa_pstream *p, uint32_t channel, int64_t offset, pa_seek_mode_t seek, const pa_memchunk *chunk, void *userdata) {
    struct userdata *u = userdata;
    assert(p && chunk && u);

    if (channel != u->channel) {
        pa_log(__FILE__": recieved memory block on bad channel.\n");
        die(u);
        return;
    }
    
    pa_source_post(u->source, chunk);
}
#endif

static void on_connection(pa_socket_client *sc, pa_iochannel *io, void *userdata) {
    struct userdata *u = userdata;
    pa_tagstruct *t;
    uint32_t tag;
    assert(sc && u && u->client == sc);

    pa_socket_client_unref(u->client);
    u->client = NULL;
    
    if (!io) {
        pa_log(__FILE__": connection failed.\n");
        pa_module_unload_request(u->module);
        return;
    }

    u->pstream = pa_pstream_new(u->core->mainloop, io, u->core->memblock_stat);
    u->pdispatch = pa_pdispatch_new(u->core->mainloop, command_table, PA_COMMAND_MAX);

    pa_pstream_set_die_callback(u->pstream, pstream_die_callback, u);
    pa_pstream_set_recieve_packet_callback(u->pstream, pstream_packet_callback, u);
#ifndef TUNNEL_SINK
    pa_pstream_set_recieve_memblock_callback(u->pstream, pstream_memblock_callback, u);
#endif
    
    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_AUTH);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_put_arbitrary(t, u->auth_cookie, sizeof(u->auth_cookie));
    pa_pstream_send_tagstruct(u->pstream, t);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, u);
    
}

#ifdef TUNNEL_SINK
static void sink_notify(pa_sink*sink) {
    struct userdata *u;
    assert(sink && sink->userdata);
    u = sink->userdata;

    send_bytes(u);
}

static pa_usec_t sink_get_latency(pa_sink *sink) {
    struct userdata *u;
    uint32_t l;
    pa_usec_t usec = 0;
    assert(sink && sink->userdata);
    u = sink->userdata;

    l = DEFAULT_TLENGTH;

    if (l > u->requested_bytes) {
        l -= u->requested_bytes;
        usec += pa_bytes_to_usec(l, &u->sink->sample_spec);
    }

    usec += u->host_latency;

    return usec;
}
#else
static pa_usec_t source_get_latency(pa_source *source) {
    struct userdata *u;
    assert(source && source->userdata);
    u = source->userdata;

    return u->host_latency;
}
#endif

static void timeout_callback(pa_mainloop_api *m, pa_time_event*e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval ntv;
    assert(m && e && u);

    request_latency(u);
    
    pa_gettimeofday(&ntv);
    ntv.tv_sec += LATENCY_INTERVAL;
    m->time_restart(e, &ntv);
}

static int load_key(struct userdata *u, const char*fn) {
    assert(u);

    u->auth_cookie_in_property = 0;
    
    if (!fn && pa_authkey_prop_get(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME, u->auth_cookie, sizeof(u->auth_cookie)) >= 0) {
        pa_log_debug(__FILE__": using already loaded auth cookie.\n");
        pa_authkey_prop_ref(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME);
        u->auth_cookie_in_property = 1;
        return 0;
    }
    
    if (!fn)
        fn = PA_NATIVE_COOKIE_FILE;

    if (pa_authkey_load_auto(fn, u->auth_cookie, sizeof(u->auth_cookie)) < 0)
        return -1;

    pa_log_debug(__FILE__": loading cookie from disk.\n");
    
    if (pa_authkey_prop_put(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME, u->auth_cookie, sizeof(u->auth_cookie)) >= 0)
        u->auth_cookie_in_property = 1;

    return 0;
}

int pa__init(pa_core *c, pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u = NULL;
    pa_sample_spec ss;
    struct timeval ntv;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments\n");
        goto fail;
    }

    u = pa_xmalloc(sizeof(struct userdata));
    m->userdata = u;
    u->module = m;
    u->core = c;
    u->client = NULL;
    u->pdispatch = NULL;
    u->pstream = NULL;
    u->server_name = NULL;
#ifdef TUNNEL_SINK
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));;
    u->sink = NULL;
    u->requested_bytes = 0;
#else
    u->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));;
    u->source = NULL;
#endif
    u->ctag = 1;
    u->device_index = u->channel = PA_INVALID_INDEX;
    u->host_latency = 0;
    u->auth_cookie_in_property = 0;
    u->time_event = NULL;
    
    if (load_key(u, pa_modargs_get_value(ma, "cookie", NULL)) < 0)
        goto fail;
    
    if (!(u->server_name = pa_xstrdup(pa_modargs_get_value(ma, "server", NULL)))) {
        pa_log(__FILE__": no server specified.\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": invalid sample format specification\n");
        goto fail;
    }

    if (!(u->client = pa_socket_client_new_string(c->mainloop, u->server_name, PA_NATIVE_DEFAULT_PORT))) {
        pa_log(__FILE__": failed to connect to server '%s'\n", u->server_name);
        goto fail;
    }
    
    if (!u->client)
        goto fail;

    pa_socket_client_set_callback(u->client, on_connection, u);

#ifdef TUNNEL_SINK
    if (!(u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, NULL))) {
        pa_log(__FILE__": failed to create sink.\n");
        goto fail;
    }

    u->sink->notify = sink_notify;
    u->sink->get_latency = sink_get_latency;
    u->sink->userdata = u;
    u->sink->description = pa_sprintf_malloc("Tunnel to '%s%s%s'", u->sink_name ? u->sink_name : "", u->sink_name ? "@" : "", u->server_name);

    pa_sink_set_owner(u->sink, m);
#else
    if (!(u->source = pa_source_new(c, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, NULL))) {
        pa_log(__FILE__": failed to create source.\n");
        goto fail;
    }

    u->source->get_latency = source_get_latency;
    u->source->userdata = u;
    u->source->description = pa_sprintf_malloc("Tunnel to '%s%s%s'", u->source_name ? u->source_name : "", u->source_name ? "@" : "", u->server_name);

    pa_source_set_owner(u->source, m);
#endif
    
    pa_gettimeofday(&ntv);
    ntv.tv_sec += LATENCY_INTERVAL;
    u->time_event = c->mainloop->time_new(c->mainloop, &ntv, timeout_callback, u);

    pa_modargs_free(ma);

    return 0;
    
fail:
    pa__done(c, m);

    if (ma)
        pa_modargs_free(ma);
    return  -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata* u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    close_stuff(u);

    if (u->auth_cookie_in_property)
        pa_authkey_prop_unref(c, PA_NATIVE_COOKIE_PROPERTY_NAME);
    
#ifdef TUNNEL_SINK
    pa_xfree(u->sink_name);
#else
    pa_xfree(u->source_name);
#endif
    pa_xfree(u->server_name);

    pa_xfree(u);
}


