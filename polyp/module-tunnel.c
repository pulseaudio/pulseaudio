/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
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
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

#include "module.h"
#include "util.h"
#include "modargs.h"
#include "log.h"
#include "subscribe.h"
#include "xmalloc.h"
#include "sink-input.h"
#include "pdispatch.h"
#include "pstream.h"
#include "pstream-util.h"
#include "authkey.h"
#include "socket-client.h"
#include "socket-util.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Tunnel module")
PA_MODULE_USAGE("server=<filename> sink=<remote sink name> cookie=<filename> format=<sample format> channels=<number of channels> rate=<sample rate> sink_name=<name for the local sink>")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define DEFAULT_SINK_NAME "tunnel"

#define DEFAULT_TLENGTH (44100*2*2/10)  //(10240*8)
#define DEFAULT_MAXLENGTH ((DEFAULT_TLENGTH*3)/2)
#define DEFAULT_MINREQ 512
#define DEFAULT_PREBUF (DEFAULT_TLENGTH-DEFAULT_MINREQ)
#define DEFAULT_FRAGSIZE 1024

#define DEFAULT_TIMEOUT 5

#define LATENCY_INTERVAL 10

static const char* const valid_modargs[] = {
    "server",
    "sink",
    "cookie",
    "format",
    "channels",
    "rate",
    "sink_name",
    NULL,
};

static void command_stream_killed(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

static const struct pa_pdispatch_command command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_REQUEST] = { command_request },
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = { command_stream_killed },
    [PA_COMMAND_RECORD_STREAM_KILLED] = { command_stream_killed },
};

struct userdata {
    struct pa_socket_client *client;
    struct pa_pstream *pstream;
    struct pa_pdispatch *pdispatch;

    char *server_name, *sink_name;
    
    struct pa_sink *sink;
    struct pa_module *module;
    struct pa_core *core;

    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];

    uint32_t ctag;
    uint32_t device_index;
    uint32_t requested_bytes;
    uint32_t channel;

    pa_usec_t host_latency;

    struct pa_time_event *time_event;
};


static void close_stuff(struct userdata *u) {
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

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }

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

static void send_prebuf_request(struct userdata *u) {
    struct pa_tagstruct *t;

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
        struct pa_memchunk chunk;
        if (pa_sink_render(u->sink, u->requested_bytes, &chunk) < 0) {

            
            if (u->requested_bytes >= DEFAULT_TLENGTH-DEFAULT_PREBUF) 
                send_prebuf_request(u);
            
            return;
        }

        pa_pstream_send_memblock(u->pstream, u->channel, 0, &chunk);
        pa_memblock_unref(chunk.memblock);

        if (chunk.length > u->requested_bytes)
            u->requested_bytes = 0;
        else
            u->requested_bytes -= chunk.length;
    }
}

static void command_stream_killed(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    assert(pd && t && u && u->pdispatch == pd);

    pa_log(__FILE__": stream killed\n");
    die(u);
}

static void command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
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

static void stream_get_latency_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    pa_usec_t buffer_usec, sink_usec, source_usec, transport_usec;
    int playing;
    uint32_t queue_length;
    struct timeval local, remote, now;
    assert(pd && u && t);

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
        !pa_tagstruct_eof(t)) {
        pa_log(__FILE__": invalid reply.\n");
        die(u);
        return;
    }

    gettimeofday(&now, NULL);

    if (pa_timeval_cmp(&local, &remote) < 0 && pa_timeval_cmp(&remote, &now)) 
        /* local and remote seem to have synchronized clocks */
        transport_usec = pa_timeval_diff(&remote, &local);
    else
        transport_usec = pa_timeval_diff(&now, &local)/2;
    
    u->host_latency = sink_usec + transport_usec;

/*     pa_log(__FILE__": estimated host latency: %0.0f usec\n", (double) u->host_latency); */
}

static void request_latency(struct userdata *u) {
    struct pa_tagstruct *t;
    struct timeval now;
    uint32_t tag;
    assert(u);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_PLAYBACK_LATENCY);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, u->channel);

    gettimeofday(&now, NULL);
    pa_tagstruct_put_timeval(t, &now);
    
    pa_pstream_send_tagstruct(u->pstream, t);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_latency_callback, u);
}

static void create_stream_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
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
        pa_tagstruct_getu32(t, &u->requested_bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_log(__FILE__": invalid reply.\n");
        die(u);
        return;
    }

    request_latency(u);
    send_bytes(u);
}

static void setup_complete_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    struct pa_tagstruct *reply;
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

    snprintf(name, sizeof(name), "Tunnel from host '%s', user '%s', sink '%s'",
             pa_get_host_name(hn, sizeof(hn)),
             pa_get_user_name(un, sizeof(un)),
             u->sink->name);
    
    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_SET_CLIENT_NAME);
    pa_tagstruct_putu32(reply, tag = u->ctag++);
    pa_tagstruct_puts(reply, name);
    pa_pstream_send_tagstruct(u->pstream, reply);
    /* We ignore the server's reply here */

    reply = pa_tagstruct_new(NULL, 0);
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
    
    pa_pstream_send_tagstruct(u->pstream, reply);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, create_stream_callback, u);
}

static void pstream_die_callback(struct pa_pstream *p, void *userdata) {
    struct userdata *u = userdata;
    assert(p && u);

    pa_log(__FILE__": stream died.\n");
    die(u);
}


static void pstream_packet_callback(struct pa_pstream *p, struct pa_packet *packet, void *userdata) {
    struct userdata *u = userdata;
    assert(p && packet && u);

    if (pa_pdispatch_run(u->pdispatch, packet, u) < 0) {
        pa_log(__FILE__": invalid packet\n");
        die(u);
    }
}

static void on_connection(struct pa_socket_client *sc, struct pa_iochannel *io, void *userdata) {
    struct userdata *u = userdata;
    struct pa_tagstruct *t;
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
    
    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_AUTH);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_put_arbitrary(t, u->auth_cookie, sizeof(u->auth_cookie));
    pa_pstream_send_tagstruct(u->pstream, t);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, u);
    
}

static void sink_notify(struct pa_sink*sink) {
    struct userdata *u;
    assert(sink && sink->userdata);
    u = sink->userdata;

    send_bytes(u);
}

static pa_usec_t sink_get_latency(struct pa_sink *sink) {
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

static void timeout_callback(struct pa_mainloop_api *m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval ntv;
    assert(m && e && u);

    request_latency(u);
    
    gettimeofday(&ntv, NULL);
    ntv.tv_sec += LATENCY_INTERVAL;
    m->time_restart(e, &ntv);
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    struct userdata *u = NULL;
    struct pa_sample_spec ss;
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
    u->server_name = u->sink_name = NULL;
    u->sink = NULL;
    u->ctag = 1;
    u->device_index = u->channel = PA_INVALID_INDEX;
    u->requested_bytes = 0;
    u->host_latency = 0;

    if (pa_authkey_load_from_home(pa_modargs_get_value(ma, "cookie", PA_NATIVE_COOKIE_FILE), u->auth_cookie, sizeof(u->auth_cookie)) < 0) {
        pa_log(__FILE__": failed to load cookie.\n");
        goto fail;
    }
    
    if (!(u->server_name = pa_xstrdup(pa_modargs_get_value(ma, "server", NULL)))) {
        pa_log(__FILE__": no server specified.\n");
        goto fail;
    }

    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": invalid sample format specification\n");
        goto fail;
    }

    if (u->server_name[0] == '/')
        u->client = pa_socket_client_new_unix(c->mainloop, u->server_name);
    else {
        size_t len; 
        struct sockaddr *sa;

        if (!(sa = pa_resolve_server(u->server_name, &len, PA_NATIVE_DEFAULT_PORT))) {
            pa_log(__FILE__": failed to resolve server '%s'\n", u->server_name);
            goto fail;
        }

        u->client = pa_socket_client_new_sockaddr(c->mainloop, sa, len);
        pa_xfree(sa);
    }
    
    if (!u->client)
        goto fail;

    pa_socket_client_set_callback(u->client, on_connection, u);
    
    if (!(u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss))) {
        pa_log(__FILE__": failed to create sink.\n");
        goto fail;
    }

    u->sink->notify = sink_notify;
    u->sink->get_latency = sink_get_latency;
    u->sink->userdata = u;
    u->sink->description = pa_sprintf_malloc("Tunnel to '%s%s%s'", u->sink_name ? u->sink_name : "", u->sink_name ? "@" : "", u->server_name);

    gettimeofday(&ntv, NULL);
    ntv.tv_sec += LATENCY_INTERVAL;
    u->time_event = c->mainloop->time_new(c->mainloop, &ntv, timeout_callback, u);

    pa_sink_set_owner(u->sink, m);

    pa_modargs_free(ma);

    return 0;
    
fail:
    pa__done(c, m);

    if (ma)
        pa_modargs_free(ma);
    return  -1;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata* u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    close_stuff(u);

    pa_xfree(u->sink_name);
    pa_xfree(u->server_name);

    pa_xfree(u);
}


