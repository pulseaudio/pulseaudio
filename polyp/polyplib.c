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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "polyplib.h"
#include "native-common.h"
#include "pdispatch.h"
#include "pstream.h"
#include "dynarray.h"
#include "socket-client.h"
#include "pstream-util.h"
#include "authkey.h"
#include "util.h"
#include "xmalloc.h"

#define DEFAULT_MAXLENGTH 204800
#define DEFAULT_TLENGTH 10240
#define DEFAULT_PREBUF 4096
#define DEFAULT_MINREQ 1024
#define DEFAULT_FRAGSIZE 1024

#define DEFAULT_TIMEOUT (5*60)
#define DEFAULT_SERVER "/tmp/polypaudio/native"
#define DEFAULT_PORT "4713"

struct pa_context {
    char *name;
    struct pa_mainloop_api* mainloop;
    struct pa_socket_client *client;
    struct pa_pstream *pstream;
    struct pa_pdispatch *pdispatch;
    struct pa_dynarray *record_streams, *playback_streams;
    struct pa_stream *first_stream;
    uint32_t ctag;
    uint32_t error;
    enum {
        CONTEXT_UNCONNECTED,
        CONTEXT_CONNECTING,
        CONTEXT_AUTHORIZING,
        CONTEXT_SETTING_NAME,
        CONTEXT_READY,
        CONTEXT_DEAD
    } state;

    void (*connect_complete_callback)(struct pa_context*c, int success, void *userdata);
    void *connect_complete_userdata;

    void (*drain_complete_callback)(struct pa_context*c, void *userdata);
    void *drain_complete_userdata;
    
    void (*die_callback)(struct pa_context*c, void *userdata);
    void *die_userdata;

    void (*stat_callback)(struct pa_context*c, uint32_t count, uint32_t total, void *userdata);
    void *stat_userdata;

    void (*play_sample_callback)(struct pa_context*c, int success, void *userdata);
    void *play_sample_userdata;
    
    void (*remove_sample_callback)(struct pa_context*c, int success, void *userdata);
    void *remove_sample_userdata;

    void (*get_server_info_callback)(struct pa_context*c, const struct pa_server_info* i, void *userdata);
    void *get_server_info_userdata;

    void (*get_sink_info_callback)(struct pa_context*c, const struct pa_sink_info* i, int is_last, void *userdata);
    void *get_sink_info_userdata;

    void (*get_source_info_callback)(struct pa_context*c, const struct pa_source_info* i, int is_last, void *userdata);
    void *get_source_info_userdata;

    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];
};

struct pa_stream {
    struct pa_context *context;
    struct pa_stream *next, *previous;

    char *name;
    struct pa_buffer_attr buffer_attr;
    struct pa_sample_spec sample_spec;
    uint32_t channel;
    int channel_valid;
    uint32_t device_index;
    enum pa_stream_direction direction;
    
    enum { STREAM_CREATING, STREAM_READY, STREAM_DEAD} state;
    uint32_t requested_bytes;

    void (*read_callback)(struct pa_stream *p, const void*data, size_t length, void *userdata);
    void *read_userdata;

    void (*write_callback)(struct pa_stream *p, size_t length, void *userdata);
    void *write_userdata;
    
    void (*create_complete_callback)(struct pa_stream *s, int success, void *userdata);
    void *create_complete_userdata;

    void (*drain_complete_callback)(struct pa_stream *s, void *userdata);
    void *drain_complete_userdata;
    
    void (*die_callback)(struct pa_stream*c, void *userdata);
    void *die_userdata;

    void (*get_latency_callback)(struct pa_stream*c, uint32_t latency, void *userdata);
    void *get_latency_userdata;

    void (*finish_sample_callback)(struct pa_stream*c, int success, void *userdata);
    void *finish_sample_userdata;
};

static void command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_playback_stream_killed(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

static const struct pa_pdispatch_command command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = { NULL },
    [PA_COMMAND_REPLY] = { NULL },
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = { NULL },
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = { NULL },
    [PA_COMMAND_CREATE_RECORD_STREAM] = { NULL },
    [PA_COMMAND_DELETE_RECORD_STREAM] = { NULL },
    [PA_COMMAND_EXIT] = { NULL },
    [PA_COMMAND_REQUEST] = { command_request },
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = { command_playback_stream_killed },
    [PA_COMMAND_RECORD_STREAM_KILLED] = { command_playback_stream_killed },
};

struct pa_context *pa_context_new(struct pa_mainloop_api *mainloop, const char *name) {
    struct pa_context *c;
    assert(mainloop && name);
    
    c = pa_xmalloc(sizeof(struct pa_context));
    c->name = pa_xstrdup(name);
    c->mainloop = mainloop;
    c->client = NULL;
    c->pstream = NULL;
    c->pdispatch = NULL;
    c->playback_streams = pa_dynarray_new();
    assert(c->playback_streams);
    c->record_streams = pa_dynarray_new();
    assert(c->record_streams);
    c->first_stream = NULL;
    c->error = PA_ERROR_OK;
    c->state = CONTEXT_UNCONNECTED;
    c->ctag = 0;

    c->connect_complete_callback = NULL;
    c->connect_complete_userdata = NULL;

    c->drain_complete_callback = NULL;
    c->drain_complete_userdata = NULL;

    c->die_callback = NULL;
    c->die_userdata = NULL;

    c->stat_callback = NULL;
    c->stat_userdata = NULL;

    c->play_sample_callback = NULL;
    c->play_sample_userdata = NULL;

    c->remove_sample_callback = NULL;
    c->remove_sample_userdata = NULL;

    c->get_server_info_callback = NULL;
    c->get_server_info_userdata = NULL;

    c->get_sink_info_callback = NULL;
    c->get_sink_info_userdata = NULL;

    c->get_source_info_callback = NULL;
    c->get_source_info_userdata = NULL;

    pa_check_for_sigpipe();
    return c;
}

void pa_context_free(struct pa_context *c) {
    assert(c);

    while (c->first_stream)
        pa_stream_free(c->first_stream);
    
    if (c->client)
        pa_socket_client_free(c->client);
    if (c->pdispatch)
        pa_pdispatch_free(c->pdispatch);
    if (c->pstream)
        pa_pstream_free(c->pstream);
    if (c->record_streams)
        pa_dynarray_free(c->record_streams, NULL, NULL);
    if (c->playback_streams)
        pa_dynarray_free(c->playback_streams, NULL, NULL);
        
    pa_xfree(c->name);
    pa_xfree(c);
}

static void stream_dead(struct pa_stream *s) {
    assert(s);
    
    if (s->state == STREAM_DEAD)
        return;
    
    if (s->state == STREAM_READY) {
        s->state = STREAM_DEAD;
        if (s->die_callback)
            s->die_callback(s, s->die_userdata);
    } else
        s->state = STREAM_DEAD;
}

static void context_dead(struct pa_context *c) {
    struct pa_stream *s;
    assert(c);
    
    if (c->state == CONTEXT_DEAD)
        return;

    if (c->pdispatch)
        pa_pdispatch_free(c->pdispatch);
    c->pdispatch = NULL;
    
    if (c->pstream)
        pa_pstream_free(c->pstream);
    c->pstream = NULL;
    
    if (c->client)
        pa_socket_client_free(c->client);
    c->client = NULL;
    
    for (s = c->first_stream; s; s = s->next)
        stream_dead(s);

    if (c->state == CONTEXT_READY) {
        c->state = CONTEXT_DEAD;
        if (c->die_callback)
            c->die_callback(c, c->die_userdata);
    } else
        c->state = CONTEXT_DEAD;
}

static void pstream_die_callback(struct pa_pstream *p, void *userdata) {
    struct pa_context *c = userdata;
    assert(p && c);
    c->error = PA_ERROR_CONNECTIONTERMINATED;
    context_dead(c);
}

static void pstream_packet_callback(struct pa_pstream *p, struct pa_packet *packet, void *userdata) {
    struct pa_context *c = userdata;
    assert(p && packet && c);

    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        fprintf(stderr, "polyp.c: invalid packet.\n");
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
    }
}

static void pstream_memblock_callback(struct pa_pstream *p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_stream *s;
    assert(p && chunk && c && chunk->memblock && chunk->memblock->data);

    if (!(s = pa_dynarray_get(c->record_streams, channel)))
        return;

    if (s->read_callback)
        s->read_callback(s, chunk->memblock->data + chunk->index, chunk->length, s->read_userdata);
}

static int handle_error(struct pa_context *c, uint32_t command, struct pa_tagstruct *t) {
    assert(c && t);
    
    if (command == PA_COMMAND_ERROR) {
        if (pa_tagstruct_getu32(t, &c->error) < 0) {
            c->error = PA_ERROR_PROTOCOL;
            return -1;
        }

        return 0;
    }

    c->error = (command == PA_COMMAND_TIMEOUT) ? PA_ERROR_TIMEOUT : PA_ERROR_INTERNAL;
    return -1;
}

static void setup_complete_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    assert(pd && c && (c->state == CONTEXT_AUTHORIZING || c->state == CONTEXT_SETTING_NAME));

    if (command != PA_COMMAND_REPLY) {
        handle_error(c, command, t);
        context_dead(c);

        if (c->connect_complete_callback)
            c->connect_complete_callback(c, 0, c->connect_complete_userdata);
        
        return;
    }

    if (c->state == CONTEXT_AUTHORIZING) {
        struct pa_tagstruct *t;
        c->state = CONTEXT_SETTING_NAME;
        t = pa_tagstruct_new(NULL, 0);
        assert(t);
        pa_tagstruct_putu32(t, PA_COMMAND_SET_NAME);
        pa_tagstruct_putu32(t, tag = c->ctag++);
        pa_tagstruct_puts(t, c->name);
        pa_pstream_send_tagstruct(c->pstream, t);
        pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, c);
    } else {
        assert(c->state == CONTEXT_SETTING_NAME);
        
        c->state = CONTEXT_READY;

        if (c->connect_complete_callback) 
            c->connect_complete_callback(c, 1, c->connect_complete_userdata);
    }

    return;
}

static void on_connection(struct pa_socket_client *client, struct pa_iochannel*io, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(client && c && c->state == CONTEXT_CONNECTING);

    pa_socket_client_free(client);
    c->client = NULL;

    if (!io) {
        c->error = PA_ERROR_CONNECTIONREFUSED;
        context_dead(c);

        if (c->connect_complete_callback)
            c->connect_complete_callback(c, 0, c->connect_complete_userdata);

        return;
    }
    
    c->pstream = pa_pstream_new(c->mainloop, io);
    assert(c->pstream);
    pa_pstream_set_die_callback(c->pstream, pstream_die_callback, c);
    pa_pstream_set_recieve_packet_callback(c->pstream, pstream_packet_callback, c);
    pa_pstream_set_recieve_memblock_callback(c->pstream, pstream_memblock_callback, c);
    
    c->pdispatch = pa_pdispatch_new(c->mainloop, command_table, PA_COMMAND_MAX);
    assert(c->pdispatch);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_AUTH);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_put_arbitrary(t, c->auth_cookie, sizeof(c->auth_cookie));
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, c);
    c->state = CONTEXT_AUTHORIZING;
}

static struct sockaddr *resolve_server(const char *server, size_t *len) {
    struct sockaddr *sa;
    struct addrinfo hints, *result = NULL;
    char *port;
    assert(server && len);

    if ((port = strrchr(server, ':')))
        port++;
    if (!port)
        port = DEFAULT_PORT;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;

    if (getaddrinfo(server, port, &hints, &result) != 0)
        return NULL;
    assert(result);
    
    sa = pa_xmalloc(*len = result->ai_addrlen);
    memcpy(sa, result->ai_addr, *len);

    freeaddrinfo(result);
    
    return sa;
}

int pa_context_connect(struct pa_context *c, const char *server, void (*complete) (struct pa_context*c, int success, void *userdata), void *userdata) {
    assert(c && c->state == CONTEXT_UNCONNECTED);

    if (pa_authkey_load_from_home(PA_NATIVE_COOKIE_FILE, c->auth_cookie, sizeof(c->auth_cookie)) < 0) {
        c->error = PA_ERROR_AUTHKEY;
        return -1;
    }

    if (!server)
        if (!(server = getenv("POLYP_SERVER")))
            server = DEFAULT_SERVER;

    assert(!c->client);
    
    if (*server == '/') {
        if (!(c->client = pa_socket_client_new_unix(c->mainloop, server))) {
            c->error = PA_ERROR_CONNECTIONREFUSED;
            return -1;
        }
    } else {
        struct sockaddr* sa;
        size_t sa_len;

        if (!(sa = resolve_server(server, &sa_len))) {
            c->error = PA_ERROR_INVALIDSERVER;
            return -1;
        }

        c->client = pa_socket_client_new_sockaddr(c->mainloop, sa, sa_len);
        pa_xfree(sa);

        if (!c->client) {
            c->error = PA_ERROR_CONNECTIONREFUSED;
            return -1;
        }
    }

    c->connect_complete_callback = complete;
    c->connect_complete_userdata = userdata;
    
    pa_socket_client_set_callback(c->client, on_connection, c);
    c->state = CONTEXT_CONNECTING;

    return 0;
}

int pa_context_is_dead(struct pa_context *c) {
    assert(c);
    return c->state == CONTEXT_DEAD;
}

int pa_context_is_ready(struct pa_context *c) {
    assert(c);
    return c->state == CONTEXT_READY;
}

int pa_context_errno(struct pa_context *c) {
    assert(c);
    return c->error;
}

void pa_context_set_die_callback(struct pa_context *c, void (*cb)(struct pa_context *c, void *userdata), void *userdata) {
    assert(c);
    c->die_callback = cb;
    c->die_userdata = userdata;
}

static void command_playback_stream_killed(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_stream *s;
    uint32_t channel;
    assert(pd && (command == PA_COMMAND_PLAYBACK_STREAM_KILLED || command == PA_COMMAND_RECORD_STREAM_KILLED) && t && c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
        return;
    }
    
    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_KILLED ? c->playback_streams : c->record_streams, channel)))
        return;

    c->error = PA_ERROR_KILLED;
    stream_dead(s);
}

static void command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s;
    struct pa_context *c = userdata;
    uint32_t bytes, channel;
    assert(pd && command == PA_COMMAND_REQUEST && t && c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
        return;
    }
    
    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        return;

    if (s->state != STREAM_READY)
        return;
    
    s->requested_bytes += bytes;

    if (s->requested_bytes && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);
}

static void create_stream_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s = userdata;
    assert(pd && s && s->state == STREAM_CREATING);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(s->context, command, t) < 0) {
            context_dead(s->context);
            return;
        }

        stream_dead(s);
        if (s->create_complete_callback)
            s->create_complete_callback(s, 0, s->create_complete_userdata);

        return;
    }

    if (pa_tagstruct_getu32(t, &s->channel) < 0 ||
        ((s->direction != PA_STREAM_UPLOAD) && pa_tagstruct_getu32(t, &s->device_index) < 0) ||
        !pa_tagstruct_eof(t)) {
        s->context->error = PA_ERROR_PROTOCOL;
        context_dead(s->context);
        return;
    }

    s->channel_valid = 1;
    pa_dynarray_put((s->direction == PA_STREAM_RECORD) ? s->context->record_streams : s->context->playback_streams, s->channel, s);
    
    s->state = STREAM_READY;
    if (s->create_complete_callback)
        s->create_complete_callback(s, 1, s->create_complete_userdata);
}

static void create_stream(struct pa_stream *s, const char *dev) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(s);

    s->state = STREAM_CREATING;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CREATE_PLAYBACK_STREAM : PA_COMMAND_CREATE_RECORD_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_put_sample_spec(t, &s->sample_spec);
    pa_tagstruct_putu32(t, (uint32_t) -1);
    pa_tagstruct_puts(t, dev ? dev : "");
    pa_tagstruct_putu32(t, s->buffer_attr.maxlength);
    if (s->direction == PA_STREAM_PLAYBACK) {
        pa_tagstruct_putu32(t, s->buffer_attr.tlength);
        pa_tagstruct_putu32(t, s->buffer_attr.prebuf);
        pa_tagstruct_putu32(t, s->buffer_attr.minreq);
    } else
        pa_tagstruct_putu32(t, s->buffer_attr.fragsize);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, create_stream_callback, s);
}

static struct pa_stream *internal_stream_new(struct pa_context *c) {
    struct pa_stream *s;

    s = pa_xmalloc(sizeof(struct pa_stream));
    s->context = c;

    s->read_callback = NULL;
    s->read_userdata = NULL;
    s->write_callback = NULL;
    s->write_userdata = NULL;
    s->die_callback = NULL;
    s->die_userdata = NULL;
    s->create_complete_callback = NULL;
    s->create_complete_userdata = NULL;
    s->get_latency_callback = NULL;
    s->get_latency_userdata = NULL;
    s->finish_sample_callback = NULL;
    s->finish_sample_userdata = NULL;

    s->name = NULL;
    s->state = STREAM_CREATING;
    s->requested_bytes = 0;
    s->channel = 0;
    s->channel_valid = 0;
    s->device_index = (uint32_t) -1;

    memset(&s->buffer_attr, 0, sizeof(s->buffer_attr));
    
    s->next = c->first_stream;
    if (s->next)
        s->next->previous = s;
    s->previous = NULL;
    c->first_stream = s;

    return s;
}

struct pa_stream* pa_stream_new(
    struct pa_context *c,
    enum pa_stream_direction dir,
    const char *dev,
    const char *name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr,
    void (*complete) (struct pa_stream*s, int success, void *userdata),
    void *userdata) {
    
    struct pa_stream *s;

    assert(c && name && ss && c->state == CONTEXT_READY && (dir == PA_STREAM_PLAYBACK || dir == PA_STREAM_RECORD));

    s = internal_stream_new(c);
    assert(s);

    s->create_complete_callback = complete;
    s->create_complete_userdata = userdata;
    s->name = pa_xstrdup(name);
    s->state = STREAM_CREATING;
    s->direction = dir;
    s->sample_spec = *ss;
    if (attr)
        s->buffer_attr = *attr;
    else {
        s->buffer_attr.maxlength = DEFAULT_MAXLENGTH;
        s->buffer_attr.tlength = DEFAULT_TLENGTH;
        s->buffer_attr.prebuf = DEFAULT_PREBUF;
        s->buffer_attr.minreq = DEFAULT_MINREQ;
        s->buffer_attr.fragsize = DEFAULT_FRAGSIZE;
    }

    create_stream(s, dev);
    
    return s;
}

void pa_stream_free(struct pa_stream *s) {
    assert(s && s->context);

    if (s->context->pdispatch) 
        pa_pdispatch_unregister_reply(s->context->pdispatch, s);
    
    pa_xfree(s->name);

    if (s->channel_valid && s->context->state == CONTEXT_READY) {
        struct pa_tagstruct *t = pa_tagstruct_new(NULL, 0);
        assert(t);
    
        pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_DELETE_PLAYBACK_STREAM :
                            (s->direction == PA_STREAM_RECORD ? PA_COMMAND_DELETE_RECORD_STREAM : PA_COMMAND_DELETE_UPLOAD_STREAM));
        pa_tagstruct_putu32(t, s->context->ctag++);
        pa_tagstruct_putu32(t, s->channel);
        pa_pstream_send_tagstruct(s->context->pstream, t);
    }
    
    if (s->channel_valid)
        pa_dynarray_put((s->direction == PA_STREAM_PLAYBACK) ? s->context->playback_streams : s->context->record_streams, s->channel, NULL);

    if (s->next)
        s->next->previous = s->previous;
    if (s->previous)
        s->previous->next = s->next;
    else
        s->context->first_stream = s->next;
    
    pa_xfree(s);
}

void pa_stream_set_write_callback(struct pa_stream *s, void (*cb)(struct pa_stream *p, size_t length, void *userdata), void *userdata) {
    s->write_callback = cb;
    s->write_userdata = userdata;
}

void pa_stream_write(struct pa_stream *s, const void *data, size_t length) {
    struct pa_memchunk chunk;
    assert(s && s->context && data && length && s->state == STREAM_READY);

    chunk.memblock = pa_memblock_new(length);
    assert(chunk.memblock && chunk.memblock->data);
    memcpy(chunk.memblock->data, data, length);
    chunk.index = 0;
    chunk.length = length;

    pa_pstream_send_memblock(s->context->pstream, s->channel, 0, &chunk);
    pa_memblock_unref(chunk.memblock);

    /*fprintf(stderr, "Sent %u bytes\n", length);*/
    
    if (length < s->requested_bytes)
        s->requested_bytes -= length;
    else
        s->requested_bytes = 0;
}

size_t pa_stream_writable_size(struct pa_stream *s) {
    assert(s && s->state == STREAM_READY);
    return s->requested_bytes;
}

void pa_stream_set_read_callback(struct pa_stream *s, void (*cb)(struct pa_stream *p, const void*data, size_t length, void *userdata), void *userdata) {
    assert(s && cb);
    s->read_callback = cb;
    s->read_userdata = userdata;
}

int pa_stream_is_dead(struct pa_stream *s) {
    return s->state == STREAM_DEAD;
}

int pa_stream_is_ready(struct pa_stream*s) {
    return s->state == STREAM_READY;
}

void pa_stream_set_die_callback(struct pa_stream *s, void (*cb)(struct pa_stream *s, void *userdata), void *userdata) {
    assert(s);
    s->die_callback = cb;
    s->die_userdata = userdata;
}

int pa_context_is_pending(struct pa_context *c) {
    assert(c);

    if (c->state != CONTEXT_READY)
        return 0;

    return pa_pstream_is_pending(c->pstream) || pa_pdispatch_is_pending(c->pdispatch);
}

struct pa_context* pa_stream_get_context(struct pa_stream *p) {
    assert(p);
    return p->context;
}

static void set_dispatch_callbacks(struct pa_context *c);

static void pdispatch_drain_callback(struct pa_pdispatch*pd, void *userdata) {
    set_dispatch_callbacks(userdata);
}

static void pstream_drain_callback(struct pa_pstream *s, void *userdata) {
    set_dispatch_callbacks(userdata);
}

static void set_dispatch_callbacks(struct pa_context *c) {
    assert(c && c->state == CONTEXT_READY);

    pa_pstream_set_drain_callback(c->pstream, NULL, NULL);
    pa_pdispatch_set_drain_callback(c->pdispatch, NULL, NULL);
    
    if (pa_pdispatch_is_pending(c->pdispatch)) {
        pa_pdispatch_set_drain_callback(c->pdispatch, pdispatch_drain_callback, c);
        return;
    }

    if (pa_pstream_is_pending(c->pstream)) {
        pa_pstream_set_drain_callback(c->pstream, pstream_drain_callback, c);
        return;
    }

    assert(c->drain_complete_callback);
    c->drain_complete_callback(c, c->drain_complete_userdata);
}

int pa_context_drain(
    struct pa_context *c, 
    void (*complete) (struct pa_context*c, void *userdata),
    void *userdata) {

    assert(c && c->state == CONTEXT_READY);

    if (complete == NULL) {
        c->drain_complete_callback = NULL;
        pa_pstream_set_drain_callback(c->pstream, NULL, NULL);
        pa_pdispatch_set_drain_callback(c->pdispatch, NULL, NULL);
        return 0;
    }
    
    if (!pa_context_is_pending(c))
        return -1;
    
    c->drain_complete_callback = complete;
    c->drain_complete_userdata = userdata;

    set_dispatch_callbacks(c);

    return 0;
}

static void stream_drain_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s = userdata;
    assert(pd && s);
    
    if (command != PA_COMMAND_REPLY) {
        if (handle_error(s->context, command, t) < 0) {
            context_dead(s->context);
            return;
        }

        stream_dead(s);
        return;
    }

    if (s->state != STREAM_READY)
        return;

    if (!pa_tagstruct_eof(t)) {
        s->context->error = PA_ERROR_PROTOCOL;
        context_dead(s->context);
        return;
    }

    if (s->drain_complete_callback) {
        void (*temp) (struct pa_stream*s, void *userdata) = s->drain_complete_callback;
        s->drain_complete_callback = NULL;
        temp(s, s->drain_complete_userdata);
    }
}


void pa_stream_drain(struct pa_stream *s, void (*complete) (struct pa_stream*s, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(s && s->state == STREAM_READY);

    if (!complete) {
        s->drain_complete_callback = NULL;
        return;
    }

    s->drain_complete_callback = complete;
    s->drain_complete_userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    
    pa_tagstruct_putu32(t, PA_COMMAND_DRAIN_PLAYBACK_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_drain_callback, s);
}

void pa_context_exit(struct pa_context *c) {
    struct pa_tagstruct *t;
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_EXIT);
    pa_tagstruct_putu32(t, c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
}

static void context_stat_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    uint32_t total, count;
    assert(pd && c);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(c, command, t) < 0) {
            context_dead(c);
            return;
        }

        if (c->stat_callback)
            c->stat_callback(c, (uint32_t) -1, (uint32_t) -1, c->stat_userdata);
        return;
    }

    if (pa_tagstruct_getu32(t, &count) < 0 ||
        pa_tagstruct_getu32(t, &total) < 0 ||
        !pa_tagstruct_eof(t)) {
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
        return;
    }

    if (c->stat_callback)
        c->stat_callback(c, count, total, c->stat_userdata);
}

void pa_context_stat(struct pa_context *c, void (*cb)(struct pa_context *c, uint32_t count, uint32_t total, void *userdata), void *userdata) {
    uint32_t tag;
    struct pa_tagstruct *t;

    c->stat_callback = cb;
    c->stat_userdata = userdata;

    if (cb == NULL)
        return;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_STAT);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_stat_callback, c);
}

static void stream_get_latency_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s = userdata;
    uint32_t latency;
    assert(pd && s);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(s->context, command, t) < 0) {
            context_dead(s->context);
            return;
        }

        if (s->get_latency_callback)
            s->get_latency_callback(s, (uint32_t) -1, s->get_latency_userdata);
        return;
    }

    if (pa_tagstruct_getu32(t, &latency) < 0 ||
        !pa_tagstruct_eof(t)) {
        s->context->error = PA_ERROR_PROTOCOL;
        context_dead(s->context);
        return;
    }

    if (s->get_latency_callback)
        s->get_latency_callback(s, latency, s->get_latency_userdata);
}

void pa_stream_get_latency(struct pa_stream *p, void (*cb)(struct pa_stream *p, uint32_t latency, void *userdata), void *userdata) {
    uint32_t tag;
    struct pa_tagstruct *t;

    p->get_latency_callback = cb;
    p->get_latency_userdata = userdata;

    if (cb == NULL)
        return;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_PLAYBACK_LATENCY);
    pa_tagstruct_putu32(t, tag = p->context->ctag++);
    pa_tagstruct_putu32(t, p->channel);
    pa_pstream_send_tagstruct(p->context->pstream, t);
    pa_pdispatch_register_reply(p->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_latency_callback, p);
}

struct pa_stream* pa_context_upload_sample(struct pa_context *c, const char *name, const struct pa_sample_spec *ss, size_t length, void (*cb) (struct pa_stream*s, int success, void *userdata), void *userdata) {
    struct pa_stream *s;
    struct pa_tagstruct *t;
    uint32_t tag;
    
    s = internal_stream_new(c);
    assert(s);

    s->create_complete_callback = cb;
    s->create_complete_userdata = userdata;
    s->name = pa_xstrdup(name);
    s->state = STREAM_CREATING;
    s->direction = PA_STREAM_UPLOAD;
    s->sample_spec = *ss;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_CREATE_UPLOAD_STREAM);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_sample_spec(t, ss);
    pa_tagstruct_putu32(t, length);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, create_stream_callback, s);

    return s;
}

static void stream_finish_sample_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s = userdata;
    assert(pd && s);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(s->context, command, t) < 0) {
            context_dead(s->context);
            return;
        }

        if (s->finish_sample_callback)
            s->finish_sample_callback(s, 0, s->finish_sample_userdata);
        return;
    }

    if (!pa_tagstruct_eof(t)) {
        s->context->error = PA_ERROR_PROTOCOL;
        context_dead(s->context);
        return;
    }

    if (s->finish_sample_callback)
        s->finish_sample_callback(s, 1, s->finish_sample_userdata);
}

void pa_stream_finish_sample(struct pa_stream *p, void (*cb)(struct pa_stream*s, int success, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(p);

    p->finish_sample_callback = cb;
    p->finish_sample_userdata = userdata;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_FINISH_UPLOAD_STREAM);
    pa_tagstruct_putu32(t, tag = p->context->ctag++);
    pa_tagstruct_putu32(t, p->channel);
    pa_pstream_send_tagstruct(p->context->pstream, t);
    pa_pdispatch_register_reply(p->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_finish_sample_callback, p);
}

static void context_play_sample_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    assert(pd && c);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(c, command, t) < 0) {
            context_dead(c);
            return;
        }

        if (c->play_sample_callback)
            c->play_sample_callback(c, 0, c->play_sample_userdata);
        return;
    }

    if (!pa_tagstruct_eof(t)) {
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
        return;
    }

    if (c->play_sample_callback)
        c->play_sample_callback(c, 1, c->play_sample_userdata);
}

void pa_context_play_sample(struct pa_context *c, const char *name, const char *dev, uint32_t volume, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c && name && *name && (!dev || *dev));

    c->play_sample_callback = cb;
    c->play_sample_userdata = userdata;

    if (!cb)
        return;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAY_SAMPLE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, (uint32_t) -1);
    pa_tagstruct_puts(t, dev ? dev : "");
    pa_tagstruct_putu32(t, volume);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_play_sample_callback, c);
}

static void context_remove_sample_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    assert(pd && c);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(c, command, t) < 0) {
            context_dead(c);
            return;
        }

        if (c->remove_sample_callback)
            c->remove_sample_callback(c, 0, c->remove_sample_userdata);
        return;
    }

    if (!pa_tagstruct_eof(t)) {
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
        return;
    }

    if (c->remove_sample_callback)
        c->remove_sample_callback(c, 1, c->remove_sample_userdata);
}

void pa_context_remove_sample(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c && name);

    c->remove_sample_callback = cb;
    c->remove_sample_userdata = userdata;
    
    if (!cb)
        return;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_REMOVE_SAMPLE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_remove_sample_callback, c);
}

static void context_get_server_info_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_server_info i;
    assert(pd && c);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(c, command, t) < 0) {
            context_dead(c);
            return;
        }

        if (c->get_server_info_callback)
            c->get_server_info_callback(c, NULL, c->get_server_info_userdata);
        return;
    }

    if (pa_tagstruct_gets(t, &i.server_name) < 0 ||
        pa_tagstruct_gets(t, &i.server_version) < 0 ||
        pa_tagstruct_gets(t, &i.user_name) < 0 ||
        pa_tagstruct_gets(t, &i.host_name) < 0 ||
        pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
        !pa_tagstruct_eof(t)) {
        c->error = PA_ERROR_PROTOCOL;
        context_dead(c);
        return;
    }

    if (c->get_server_info_callback)
        c->get_server_info_callback(c, &i, c->get_server_info_userdata);
}

void pa_context_get_server_info(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_server_info*i, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c);

    c->get_server_info_callback = cb;
    c->get_server_info_userdata = userdata;

    if (!cb)
        return;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SERVER_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_server_info_callback, c);
}

static void context_get_sink_info_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    assert(pd && c);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(c, command, t) < 0) {
            context_dead(c);
            return;
        }

        if (c->get_sink_info_callback)
            c->get_sink_info_callback(c, NULL, 0, c->get_sink_info_userdata);
        return;
    }

    while (!pa_tagstruct_eof(t)) {
        struct pa_sink_info i;
        
        if (pa_tagstruct_getu32(t, &i.index) < 0 ||
            pa_tagstruct_gets(t, &i.name) < 0 ||
            pa_tagstruct_gets(t, &i.description) < 0 ||
            pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
            pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
            pa_tagstruct_getu32(t, &i.volume) < 0 ||
            pa_tagstruct_getu32(t, &i.monitor_source) < 0 ||
            pa_tagstruct_gets(t, &i.monitor_source_name) < 0 ||
            pa_tagstruct_getu32(t, &i.latency) < 0) {
            c->error = PA_ERROR_PROTOCOL;
            context_dead(c);
            return;
        }
        
        if (c->get_sink_info_callback)
            c->get_sink_info_callback(c, &i, 0, c->get_sink_info_userdata);
    }

    if (c->get_sink_info_callback)
        c->get_sink_info_callback(c, NULL, 1, c->get_sink_info_userdata);
}

void pa_context_get_sink_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, int is_last, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c);

    c->get_sink_info_callback = cb;
    c->get_sink_info_userdata = userdata;

    if (!cb)
        return;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SINK_INFO_LIST);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_info_callback, c);
}

static void context_get_source_info_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    assert(pd && c);

    if (command != PA_COMMAND_REPLY) {
        if (handle_error(c, command, t) < 0) {
            context_dead(c);
            return;
        }

        if (c->get_source_info_callback)
            c->get_source_info_callback(c, NULL, 0, c->get_source_info_userdata);
        return;
    }

    while (!pa_tagstruct_eof(t)) {
        struct pa_source_info i;

        if (pa_tagstruct_getu32(t, &i.index) < 0 ||
            pa_tagstruct_gets(t, &i.name) < 0 ||
            pa_tagstruct_gets(t, &i.description) < 0 ||
            pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
            pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
            pa_tagstruct_getu32(t, &i.monitor_of_sink) < 0 ||
            pa_tagstruct_gets(t, &i.monitor_of_sink_name) < 0) {
            c->error = PA_ERROR_PROTOCOL;
            context_dead(c);
            return;
        }
        
        if (c->get_source_info_callback)
            c->get_source_info_callback(c, &i, 0, c->get_source_info_userdata);
    }

    if (c->get_source_info_callback)
        c->get_source_info_callback(c, NULL, 1, c->get_source_info_userdata);
}

void pa_context_get_source_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_source_info *i, int is_last, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c);

    c->get_source_info_callback = cb;
    c->get_source_info_userdata = userdata;

    if (!cb)
        return;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SOURCE_INFO_LIST);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_info_callback, c);
}
