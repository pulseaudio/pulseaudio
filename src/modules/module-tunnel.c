/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/version.h>
#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/pdispatch.h>
#include <pulsecore/pstream.h>
#include <pulsecore/pstream-util.h>
#include <pulsecore/authkey.h>
#include <pulsecore/socket-client.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/authkey-prop.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/core-error.h>

#ifdef TUNNEL_SINK
#include "module-tunnel-sink-symdef.h"
PA_MODULE_DESCRIPTION("Tunnel module for sinks")
PA_MODULE_USAGE(
        "server=<address> "
        "sink=<remote sink name> "
        "cookie=<filename> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "sink_name=<name for the local sink> "
        "channel_map=<channel map>")
#else
#include "module-tunnel-source-symdef.h"
PA_MODULE_DESCRIPTION("Tunnel module for sources")
PA_MODULE_USAGE(
        "server=<address> "
        "source=<remote source name> "
        "cookie=<filename> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "source_name=<name for the local source> "
        "channel_map=<channel map>")
#endif

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define DEFAULT_TLENGTH_MSEC 100
#define DEFAULT_MINREQ_MSEC 10
#define DEFAULT_MAXLENGTH_MSEC ((DEFAULT_TLENGTH_MSEC*3)/2)
#define DEFAULT_FRAGSIZE_MSEC 10

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
    "channel_map",
    NULL,
};

enum {
    SOURCE_MESSAGE_POST = PA_SOURCE_MESSAGE_MAX
};

enum {
    SINK_MESSAGE_REQUEST = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_POST
};

#ifdef TUNNEL_SINK
static void command_request(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_subscribe_event(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
#endif
static void command_stream_killed(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_overflow(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_underflow(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

static const pa_pdispatch_cb_t command_table[PA_COMMAND_MAX] = {
#ifdef TUNNEL_SINK
    [PA_COMMAND_REQUEST] = command_request,
    [PA_COMMAND_SUBSCRIBE_EVENT] = command_subscribe_event,
#endif
    [PA_COMMAND_OVERFLOW] = command_overflow,
    [PA_COMMAND_UNDERFLOW] = command_underflow,
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = command_stream_killed,
    [PA_COMMAND_RECORD_STREAM_KILLED] = command_stream_killed,
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_thread *thread;

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

    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];

    uint32_t version;
    uint32_t ctag;
    uint32_t device_index;
    uint32_t channel;

    int64_t counter, counter_delta;

    pa_time_event *time_event;

    pa_bool_t auth_cookie_in_property;

    pa_smoother *smoother;

    uint32_t maxlength;
#ifdef TUNNEL_SINK
    uint32_t tlength;
    uint32_t minreq;
    uint32_t prebuf;
#else
    uint32_t fragsize;
#endif
};

static void command_stream_killed(pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(pd);
    pa_assert(t);
    pa_assert(u);
    pa_assert(u->pdispatch == pd);

    pa_log_warn("Stream killed");
    pa_module_unload_request(u->module);
}

static void command_overflow(pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(pd);
    pa_assert(t);
    pa_assert(u);
    pa_assert(u->pdispatch == pd);

    pa_log_warn("Server signalled buffer overrun.");
}

static void command_underflow(pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(pd);
    pa_assert(t);
    pa_assert(u);
    pa_assert(u->pdispatch == pd);

    pa_log_warn("Server signalled buffer underrun.");
}

static void stream_cork(struct userdata *u, pa_bool_t cork) {
    pa_tagstruct *t;
    pa_assert(u);

    if (cork)
        pa_smoother_pause(u->smoother, pa_rtclock_usec());
    else
        pa_smoother_resume(u->smoother, pa_rtclock_usec());

    if (!u->pstream)
        return;

    t = pa_tagstruct_new(NULL, 0);
#ifdef TUNNEL_SINK
    pa_tagstruct_putu32(t, PA_COMMAND_CORK_PLAYBACK_STREAM);
#else
    pa_tagstruct_putu32(t, PA_COMMAND_CORK_RECORD_STREAM);
#endif
    pa_tagstruct_putu32(t, u->ctag++);
    pa_tagstruct_putu32(t, u->channel);
    pa_tagstruct_put_boolean(t, !!cork);
    pa_pstream_send_tagstruct(u->pstream, t);
}

#ifdef TUNNEL_SINK

static void send_data(struct userdata *u) {
    pa_assert(u);

    while (u->requested_bytes > 0) {
        pa_memchunk memchunk;
        pa_sink_render(u->sink, u->requested_bytes, &memchunk);
        pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_POST, NULL, 0, &memchunk, NULL);
        pa_memblock_unref(memchunk.memblock);
        u->requested_bytes -= memchunk.length;
    }
}

/* This function is called from IO context -- except when it is not. */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_SET_STATE: {
            int r;

            /* First, change the state, because otherwide pa_sink_render() would fail */
            if ((r = pa_sink_process_msg(o, code, data, offset, chunk)) >= 0)
                if (PA_SINK_OPENED((pa_sink_state_t) PA_PTR_TO_UINT(data)))
                    send_data(u);

            return r;
        }

        case SINK_MESSAGE_REQUEST:

            pa_assert(offset > 0);
            u->requested_bytes += (size_t) offset;

            if (PA_SINK_OPENED(u->sink->thread_info.state))
                send_data(u);

            return 0;

        case SINK_MESSAGE_POST:

            /* OK, This might be a bit confusing. This message is
             * delivered to us from the main context -- NOT from the
             * IO thread context where the rest of the messages are
             * dispatched. Yeah, ugly, but I am a lazy bastard. */

            pa_pstream_send_memblock(u->pstream, u->channel, 0, PA_SEEK_RELATIVE, chunk);
            u->counter += chunk->length;
            u->counter_delta += chunk->length;
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;
    pa_sink_assert_ref(s);
    u = s->userdata;

    switch ((pa_sink_state_t) state) {

        case PA_SINK_SUSPENDED:
            pa_assert(PA_SINK_OPENED(s->state));
            stream_cork(u, TRUE);
            break;

        case PA_SINK_IDLE:
        case PA_SINK_RUNNING:
            if (s->state == PA_SINK_SUSPENDED)
                stream_cork(u, FALSE);
            break;

        case PA_SINK_UNLINKED:
        case PA_SINK_INIT:
            ;
    }

    return 0;
}

#else

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {
        case SOURCE_MESSAGE_POST:

            if (PA_SOURCE_OPENED(u->source->thread_info.state))
                pa_source_post(u->source, chunk);
            return 0;
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int source_set_state(pa_source *s, pa_source_state_t state) {
    struct userdata *u;
    pa_source_assert_ref(s);
    u = s->userdata;

    switch ((pa_source_state_t) state) {

        case PA_SOURCE_SUSPENDED:
            pa_assert(PA_SOURCE_OPENED(s->state));
            stream_cork(u, TRUE);
            break;

        case PA_SOURCE_IDLE:
        case PA_SOURCE_RUNNING:
            if (s->state == PA_SOURCE_SUSPENDED)
                stream_cork(u, FALSE);
            break;

        case PA_SOURCE_UNLINKED:
        case PA_SOURCE_INIT:
            ;
    }

    return 0;
}

#endif

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    for (;;) {
        int ret;

        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

#ifdef TUNNEL_SINK
static void command_request(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    uint32_t bytes, channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_REQUEST);
    pa_assert(t);
    pa_assert(u);
    pa_assert(u->pdispatch == pd);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_log("Invalid protocol reply");
        goto fail;
    }

    if (channel != u->channel) {
        pa_log("Recieved data for invalid channel");
        goto fail;
    }

    pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_REQUEST, NULL, bytes, NULL);
    return;

fail:
    pa_module_unload_request(u->module);
}

#endif

static void stream_get_latency_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    pa_usec_t sink_usec, source_usec, transport_usec, host_usec, k;
    int playing;
    int64_t write_index, read_index;
    struct timeval local, remote, now;

    pa_assert(pd);
    pa_assert(u);

    if (command != PA_COMMAND_REPLY) {
        if (command == PA_COMMAND_ERROR)
            pa_log("Failed to get latency.");
        else
            pa_log("Protocol error.");
        goto fail;
    }

    if (pa_tagstruct_get_usec(t, &sink_usec) < 0 ||
        pa_tagstruct_get_usec(t, &source_usec) < 0 ||
        pa_tagstruct_get_boolean(t, &playing) < 0 ||
        pa_tagstruct_get_timeval(t, &local) < 0 ||
        pa_tagstruct_get_timeval(t, &remote) < 0 ||
        pa_tagstruct_gets64(t, &write_index) < 0 ||
        pa_tagstruct_gets64(t, &read_index) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_log("Invalid reply. (latency)");
        goto fail;
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
    host_usec = sink_usec + transport_usec;
#else
    host_usec = source_usec + transport_usec;
    if (host_usec > sink_usec)
        host_usec -= sink_usec;
    else
        host_usec = 0;
#endif

#ifdef TUNNEL_SINK
    k = pa_bytes_to_usec(u->counter - u->counter_delta, &u->sink->sample_spec);

    if (k > host_usec)
        k -= host_usec;
    else
        k = 0;
#else
    k = pa_bytes_to_usec(u->counter - u->counter_delta, &u->source->sample_spec);
    k += host_usec;
#endif

    pa_smoother_put(u->smoother, pa_rtclock_usec(), k);

    return;

fail:
    pa_module_unload_request(u->module);
}

static void request_latency(struct userdata *u) {
    pa_tagstruct *t;
    struct timeval now;
    uint32_t tag;
    pa_assert(u);

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

    pa_pstream_send_tagstruct(u->pstream, t);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_latency_callback, u, NULL);

    u->counter_delta = 0;
}

static void timeout_callback(pa_mainloop_api *m, pa_time_event*e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval ntv;

    pa_assert(m);
    pa_assert(e);
    pa_assert(u);

    request_latency(u);

    pa_gettimeofday(&ntv);
    ntv.tv_sec += LATENCY_INTERVAL;
    m->time_restart(e, &ntv);
}

#ifdef TUNNEL_SINK
static pa_usec_t sink_get_latency(pa_sink *s) {
    pa_usec_t t, c;
    struct userdata *u = s->userdata;

    pa_sink_assert_ref(s);

    c = pa_bytes_to_usec(u->counter, &s->sample_spec);
    t = pa_smoother_get(u->smoother, pa_rtclock_usec());

    return c > t ? c - t : 0;
}
#else
static pa_usec_t source_get_latency(pa_source *s) {
    pa_usec_t t, c;
    struct userdata *u = s->userdata;

    pa_source_assert_ref(s);

    c = pa_bytes_to_usec(u->counter, &s->sample_spec);
    t = pa_smoother_get(u->smoother, pa_rtclock_usec());

    return t > c ? t - c : 0;
}
#endif

#ifdef TUNNEL_SINK

static void sink_input_info_cb(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    uint32_t idx, owner_module, client, sink;
    pa_usec_t buffer_usec, sink_usec;
    const char *name, *driver, *resample_method;
    int mute;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_cvolume volume;

    pa_assert(pd);
    pa_assert(u);

    if (command != PA_COMMAND_REPLY) {
        if (command == PA_COMMAND_ERROR)
            pa_log("Failed to get info.");
        else
            pa_log("Protocol error.");
        goto fail;
    }

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_getu32(t, &owner_module) < 0 ||
        pa_tagstruct_getu32(t, &client) < 0 ||
        pa_tagstruct_getu32(t, &sink) < 0 ||
        pa_tagstruct_get_sample_spec(t, &sample_spec) < 0 ||
        pa_tagstruct_get_channel_map(t, &channel_map) < 0 ||
        pa_tagstruct_get_cvolume(t, &volume) < 0 ||
        pa_tagstruct_get_usec(t, &buffer_usec) < 0 ||
        pa_tagstruct_get_usec(t, &sink_usec) < 0 ||
        pa_tagstruct_gets(t, &resample_method) < 0 ||
        pa_tagstruct_gets(t, &driver) < 0 ||
        (u->version >= 11 && pa_tagstruct_get_boolean(t, &mute) < 0) ||
        !pa_tagstruct_eof(t)) {
        pa_log("Invalid reply. (get_info)");
        goto fail;
    }

    pa_assert(u->sink);

    if ((u->version < 11 || !!mute == !!u->sink->muted) &&
        pa_cvolume_equal(&volume, &u->sink->volume))
        return;

    memcpy(&u->sink->volume, &volume, sizeof(pa_cvolume));

    if (u->version >= 11)
        u->sink->muted = !!mute;

    pa_subscription_post(u->sink->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, u->sink->index);
    return;

fail:
    pa_module_unload_request(u->module);
}

static void request_info(struct userdata *u) {
    pa_tagstruct *t;
    uint32_t tag;
    pa_assert(u);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SINK_INPUT_INFO);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, u->device_index);
    pa_pstream_send_tagstruct(u->pstream, t);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, sink_input_info_cb, u, NULL);
}

static void command_subscribe_event(pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    pa_subscription_event_type_t e;
    uint32_t idx;

    pa_assert(pd);
    pa_assert(t);
    pa_assert(u);
    pa_assert(command == PA_COMMAND_SUBSCRIBE_EVENT);

    if (pa_tagstruct_getu32(t, &e) < 0 ||
        pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_log("Invalid protocol reply");
        pa_module_unload_request(u->module);
        return;
    }

    if (e != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    request_info(u);
}

static void start_subscribe(struct userdata *u) {
    pa_tagstruct *t;
    uint32_t tag;
    pa_assert(u);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SUBSCRIBE);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, PA_SUBSCRIPTION_MASK_SINK_INPUT);
    pa_pstream_send_tagstruct(u->pstream, t);
}
#endif

static void create_stream_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    struct timeval ntv;
#ifdef TUNNEL_SINK
    uint32_t bytes;
#endif

    pa_assert(pd);
    pa_assert(u);
    pa_assert(u->pdispatch == pd);

    if (command != PA_COMMAND_REPLY) {
        if (command == PA_COMMAND_ERROR)
            pa_log("Failed to create stream.");
        else
            pa_log("Protocol error.");
        goto fail;
    }

    if (pa_tagstruct_getu32(t, &u->channel) < 0 ||
        pa_tagstruct_getu32(t, &u->device_index) < 0
#ifdef TUNNEL_SINK
        || pa_tagstruct_getu32(t, &bytes) < 0
#endif
        )
        goto parse_error;

    if (u->version >= 9) {
#ifdef TUNNEL_SINK
        uint32_t maxlength, tlength, prebuf, minreq;

        if (pa_tagstruct_getu32(t, &maxlength) < 0 ||
            pa_tagstruct_getu32(t, &tlength) < 0 ||
            pa_tagstruct_getu32(t, &prebuf) < 0 ||
            pa_tagstruct_getu32(t, &minreq) < 0)
            goto parse_error;
#else
        uint32_t maxlength, fragsize;

        if (pa_tagstruct_getu32(t, &maxlength) < 0 ||
            pa_tagstruct_getu32(t, &fragsize) < 0)
            goto parse_error;
#endif
    }

    if (!pa_tagstruct_eof(t))
        goto parse_error;

#ifdef TUNNEL_SINK
    start_subscribe(u);
    request_info(u);
#endif

    pa_assert(!u->time_event);
    pa_gettimeofday(&ntv);
    ntv.tv_sec += LATENCY_INTERVAL;
    u->time_event = u->core->mainloop->time_new(u->core->mainloop, &ntv, timeout_callback, u);

    request_latency(u);

    pa_log_debug("Stream created.");

#ifdef TUNNEL_SINK
    pa_asyncmsgq_post(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_REQUEST, NULL, bytes, NULL, NULL);
#endif

    return;

parse_error:
    pa_log("Invalid reply. (Create stream)");

fail:
    pa_module_unload_request(u->module);
}

static void setup_complete_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct userdata *u = userdata;
    pa_tagstruct *reply;
    char name[256], un[128], hn[128];
#ifdef TUNNEL_SINK
    pa_cvolume volume;
#endif

    pa_assert(pd);
    pa_assert(u);
    pa_assert(u->pdispatch == pd);

    if (command != PA_COMMAND_REPLY ||
        pa_tagstruct_getu32(t, &u->version) < 0 ||
        !pa_tagstruct_eof(t)) {
        if (command == PA_COMMAND_ERROR)
            pa_log("Failed to authenticate");
        else
            pa_log("Protocol error.");

        goto fail;
    }

    /* Minimum supported protocol version */
    if (u->version < 8) {
        pa_log("Incompatible protocol version");
        goto fail;
    }

#ifdef TUNNEL_SINK
    pa_snprintf(name, sizeof(name), "Tunnel from host %s, user %s, sink %s",
             pa_get_host_name(hn, sizeof(hn)),
             pa_get_user_name(un, sizeof(un)),
             u->sink->name);
#else
    pa_snprintf(name, sizeof(name), "Tunnel from host %s, user %s, source %s",
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
    pa_tagstruct_put_channel_map(reply, &u->sink->channel_map);
    pa_tagstruct_putu32(reply, PA_INVALID_INDEX);
    pa_tagstruct_puts(reply, u->sink_name);
    pa_tagstruct_putu32(reply, u->maxlength);
    pa_tagstruct_put_boolean(reply, !PA_SINK_OPENED(pa_sink_get_state(u->sink)));
    pa_tagstruct_putu32(reply, u->tlength);
    pa_tagstruct_putu32(reply, u->prebuf);
    pa_tagstruct_putu32(reply, u->minreq);
    pa_tagstruct_putu32(reply, 0);
    pa_cvolume_reset(&volume, u->sink->sample_spec.channels);
    pa_tagstruct_put_cvolume(reply, &volume);
#else
    pa_tagstruct_putu32(reply, PA_COMMAND_CREATE_RECORD_STREAM);
    pa_tagstruct_putu32(reply, tag = u->ctag++);
    pa_tagstruct_puts(reply, name);
    pa_tagstruct_put_sample_spec(reply, &u->source->sample_spec);
    pa_tagstruct_put_channel_map(reply, &u->source->channel_map);
    pa_tagstruct_putu32(reply, PA_INVALID_INDEX);
    pa_tagstruct_puts(reply, u->source_name);
    pa_tagstruct_putu32(reply, u->maxlength);
    pa_tagstruct_put_boolean(reply, !PA_SOURCE_OPENED(pa_source_get_state(u->source)));
    pa_tagstruct_putu32(reply, u->fragsize);
#endif

    pa_pstream_send_tagstruct(u->pstream, reply);
    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, create_stream_callback, u, NULL);

    pa_log_debug("Connection authenticated, creating stream ...");

    return;

fail:
    pa_module_unload_request(u->module);
}

static void pstream_die_callback(pa_pstream *p, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(p);
    pa_assert(u);

    pa_log_warn("Stream died.");
    pa_module_unload_request(u->module);
}

static void pstream_packet_callback(pa_pstream *p, pa_packet *packet, const pa_creds *creds, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(p);
    pa_assert(packet);
    pa_assert(u);

    if (pa_pdispatch_run(u->pdispatch, packet, creds, u) < 0) {
        pa_log("Invalid packet");
        pa_module_unload_request(u->module);
        return;
    }
}

#ifndef TUNNEL_SINK
static void pstream_memblock_callback(pa_pstream *p, uint32_t channel, int64_t offset, pa_seek_mode_t seek, const pa_memchunk *chunk, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(p);
    pa_assert(chunk);
    pa_assert(u);

    if (channel != u->channel) {
        pa_log("Recieved memory block on bad channel.");
        pa_module_unload_request(u->module);
        return;
    }

    pa_asyncmsgq_send(u->source->asyncmsgq, PA_MSGOBJECT(u->source), SOURCE_MESSAGE_POST, PA_UINT_TO_PTR(seek), offset, chunk);

    u->counter += chunk->length;
    u->counter_delta += chunk->length;
}

#endif

static void on_connection(pa_socket_client *sc, pa_iochannel *io, void *userdata) {
    struct userdata *u = userdata;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(sc);
    pa_assert(u);
    pa_assert(u->client == sc);

    pa_socket_client_unref(u->client);
    u->client = NULL;

    if (!io) {
        pa_log("Connection failed: %s", pa_cstrerror(errno));
        pa_module_unload_request(u->module);
        return;
    }

    u->pstream = pa_pstream_new(u->core->mainloop, io, u->core->mempool);
    u->pdispatch = pa_pdispatch_new(u->core->mainloop, command_table, PA_COMMAND_MAX);

    pa_pstream_set_die_callback(u->pstream, pstream_die_callback, u);
    pa_pstream_set_recieve_packet_callback(u->pstream, pstream_packet_callback, u);
#ifndef TUNNEL_SINK
    pa_pstream_set_recieve_memblock_callback(u->pstream, pstream_memblock_callback, u);
#endif

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_AUTH);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, PA_PROTOCOL_VERSION);
    pa_tagstruct_put_arbitrary(t, u->auth_cookie, sizeof(u->auth_cookie));

#ifdef HAVE_CREDS
{
    pa_creds ucred;

    if (pa_iochannel_creds_supported(io))
        pa_iochannel_creds_enable(io);

    ucred.uid = getuid();
    ucred.gid = getgid();

    pa_pstream_send_tagstruct_with_creds(u->pstream, t, &ucred);
}
#else
    pa_pstream_send_tagstruct(u->pstream, t);
#endif

    pa_pdispatch_register_reply(u->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, u, NULL);

    pa_log_debug("Connection established, authenticating ...");
}

#ifdef TUNNEL_SINK

static int sink_get_volume(pa_sink *sink) {
    return 0;
}

static int sink_set_volume(pa_sink *sink) {
    struct userdata *u;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(sink);
    u = sink->userdata;
    pa_assert(u);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_SINK_INPUT_VOLUME);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, u->device_index);
    pa_tagstruct_put_cvolume(t, &sink->volume);
    pa_pstream_send_tagstruct(u->pstream, t);

    return 0;
}

static int sink_get_mute(pa_sink *sink) {
    return 0;
}

static int sink_set_mute(pa_sink *sink) {
    struct userdata *u;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(sink);
    u = sink->userdata;
    pa_assert(u);

    if (u->version < 11)
        return -1;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_SINK_INPUT_MUTE);
    pa_tagstruct_putu32(t, tag = u->ctag++);
    pa_tagstruct_putu32(t, u->device_index);
    pa_tagstruct_put_boolean(t, !!sink->muted);
    pa_pstream_send_tagstruct(u->pstream, t);

    return 0;
}

#endif

static int load_key(struct userdata *u, const char*fn) {
    pa_assert(u);

    u->auth_cookie_in_property = FALSE;

    if (!fn && pa_authkey_prop_get(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME, u->auth_cookie, sizeof(u->auth_cookie)) >= 0) {
        pa_log_debug("using already loaded auth cookie.");
        pa_authkey_prop_ref(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME);
        u->auth_cookie_in_property = 1;
        return 0;
    }

    if (!fn)
        fn = PA_NATIVE_COOKIE_FILE;

    if (pa_authkey_load_auto(fn, u->auth_cookie, sizeof(u->auth_cookie)) < 0)
        return -1;

    pa_log_debug("loading cookie from disk.");

    if (pa_authkey_prop_put(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME, u->auth_cookie, sizeof(u->auth_cookie)) >= 0)
        u->auth_cookie_in_property = TRUE;

    return 0;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    char *t, *dn = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    u = pa_xnew(struct userdata, 1);
    m->userdata = u;
    u->module = m;
    u->core = m->core;
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
    u->smoother = pa_smoother_new(PA_USEC_PER_SEC, PA_USEC_PER_SEC*2, TRUE);
    u->ctag = 1;
    u->device_index = u->channel = PA_INVALID_INDEX;
    u->auth_cookie_in_property = FALSE;
    u->time_event = NULL;

    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);
    u->rtpoll = pa_rtpoll_new();
    pa_rtpoll_item_new_asyncmsgq(u->rtpoll, PA_RTPOLL_EARLY, u->thread_mq.inq);

    if (load_key(u, pa_modargs_get_value(ma, "cookie", NULL)) < 0)
        goto fail;

    if (!(u->server_name = pa_xstrdup(pa_modargs_get_value(ma, "server", NULL)))) {
        pa_log("no server specified.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("invalid sample format specification");
        goto fail;
    }

    if (!(u->client = pa_socket_client_new_string(m->core->mainloop, u->server_name, PA_NATIVE_DEFAULT_PORT))) {
        pa_log("failed to connect to server '%s'", u->server_name);
        goto fail;
    }

    pa_socket_client_set_callback(u->client, on_connection, u);

#ifdef TUNNEL_SINK

    if (!(dn = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        dn = pa_sprintf_malloc("tunnel.%s", u->server_name);

    if (!(u->sink = pa_sink_new(m->core, __FILE__, dn, 1, &ss, &map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;
    u->sink->set_state = sink_set_state;
    u->sink->get_latency = sink_get_latency;
    u->sink->get_volume = sink_get_volume;
    u->sink->get_mute = sink_get_mute;
    u->sink->set_volume = sink_set_volume;
    u->sink->set_mute = sink_set_mute;
    u->sink->flags = PA_SINK_NETWORK|PA_SINK_LATENCY|PA_SINK_HW_VOLUME_CTRL;

    pa_sink_set_module(u->sink, m);
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Tunnel to %s%s%s", u->sink_name ? u->sink_name : "", u->sink_name ? " on " : "", u->server_name));
    pa_xfree(t);

#else

    if (!(dn = pa_xstrdup(pa_modargs_get_value(ma, "source_name", NULL))))
        dn = pa_sprintf_malloc("tunnel.%s", u->server_name);

    if (!(u->source = pa_source_new(m->core, __FILE__, dn, 1, &ss, &map))) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
    u->source->userdata = u;
    u->source->set_state = source_set_state;
    u->source->get_latency = source_get_latency;
    u->source->flags = PA_SOURCE_NETWORK|PA_SOURCE_LATENCY;

    pa_source_set_module(u->source, m);
    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_description(u->source, t = pa_sprintf_malloc("Tunnel to %s%s%s", u->source_name ? u->source_name : "", u->source_name ? " on " : "", u->server_name));
    pa_xfree(t);
#endif

    pa_xfree(dn);

    u->time_event = NULL;

    u->maxlength = pa_usec_to_bytes(PA_USEC_PER_MSEC * DEFAULT_MAXLENGTH_MSEC, &ss);
#ifdef TUNNEL_SINK
    u->tlength = pa_usec_to_bytes(PA_USEC_PER_MSEC * DEFAULT_TLENGTH_MSEC, &ss);
    u->minreq = pa_usec_to_bytes(PA_USEC_PER_MSEC * DEFAULT_MINREQ_MSEC, &ss);
    u->prebuf = u->tlength;
#else
    u->fragsize = pa_usec_to_bytes(PA_USEC_PER_MSEC * DEFAULT_FRAGSIZE_MSEC, &ss);
#endif

    u->counter = u->counter_delta = 0;
    pa_smoother_set_time_offset(u->smoother, pa_rtclock_usec());

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

#ifdef TUNNEL_SINK
    pa_sink_put(u->sink);
#else
    pa_source_put(u->source);
#endif

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    pa_xfree(dn);

    return  -1;
}

void pa__done(pa_module*m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

#ifdef TUNNEL_SINK
    if (u->sink)
        pa_sink_unlink(u->sink);
#else
    if (u->source)
        pa_source_unlink(u->source);
#endif

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

#ifdef TUNNEL_SINK
    if (u->sink)
        pa_sink_unref(u->sink);
#else
    if (u->source)
        pa_source_unref(u->source);
#endif

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->pstream) {
        pa_pstream_unlink(u->pstream);
        pa_pstream_unref(u->pstream);
    }

    if (u->pdispatch)
        pa_pdispatch_unref(u->pdispatch);

    if (u->client)
        pa_socket_client_unref(u->client);

    if (u->auth_cookie_in_property)
        pa_authkey_prop_unref(m->core, PA_NATIVE_COOKIE_PROPERTY_NAME);

    if (u->smoother)
        pa_smoother_free(u->smoother);

    if (u->time_event)
        u->core->mainloop->time_free(u->time_event);

#ifdef TUNNEL_SINK
    pa_xfree(u->sink_name);
#else
    pa_xfree(u->source_name);
#endif
    pa_xfree(u->server_name);

    pa_xfree(u);
}
