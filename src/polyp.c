#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "polyp.h"
#include "protocol-native-spec.h"
#include "pdispatch.h"
#include "pstream.h"
#include "dynarray.h"
#include "socket-client.h"
#include "pstream-util.h"

#define DEFAULT_QUEUE_LENGTH 10240
#define DEFAULT_MAX_LENGTH 20480
#define DEFAULT_PREBUF 4096
#define DEFAULT_TIMEOUT (5*60)
#define DEFAULT_SERVER "/tmp/polypaudio/native"

struct pa_context {
    char *name;
    struct pa_mainloop_api* mainloop;
    struct pa_socket_client *client;
    struct pa_pstream *pstream;
    struct pa_pdispatch *pdispatch;
    struct pa_dynarray *streams;
    struct pa_stream *first_stream;
    uint32_t ctag;
    uint32_t errno;
    enum { CONTEXT_UNCONNECTED, CONTEXT_CONNECTING, CONTEXT_READY, CONTEXT_DEAD} state;

    void (*connect_complete_callback)(struct pa_context*c, int success, void *userdata);
    void *connect_complete_userdata;

    void (*die_callback)(struct pa_context*c, void *userdata);
    void *die_userdata;
};

struct pa_stream {
    struct pa_context *context;
    struct pa_stream *next, *previous;
    uint32_t device_index;
    uint32_t channel;
    int channel_valid;
    enum pa_stream_direction direction;
    enum { STREAM_CREATING, STREAM_READY, STREAM_DEAD} state;
    uint32_t requested_bytes;

    void (*read_callback)(struct pa_stream *p, const void*data, size_t length, void *userdata);
    void *read_userdata;

    void (*write_callback)(struct pa_stream *p, size_t length, void *userdata);
    void *write_userdata;
    
    void (*create_complete_callback)(struct pa_context*c, struct pa_stream *s, void *userdata);
    void *create_complete_userdata;
    
    void (*die_callback)(struct pa_stream*c, void *userdata);
    void *die_userdata;
};

static int command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

static const struct pa_pdispatch_command command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = { NULL },
    [PA_COMMAND_REPLY] = { NULL },
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = { NULL },
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = { NULL },
    [PA_COMMAND_CREATE_RECORD_STREAM] = { NULL },
    [PA_COMMAND_DELETE_RECORD_STREAM] = { NULL },
    [PA_COMMAND_EXIT] = { NULL },
    [PA_COMMAND_REQUEST] = { command_request },
};

struct pa_context *pa_context_new(struct pa_mainloop_api *mainloop, const char *name) {
    assert(mainloop && name);
    struct pa_context *c;
    c = malloc(sizeof(struct pa_context));
    assert(c);
    c->name = strdup(name);
    c->mainloop = mainloop;
    c->client = NULL;
    c->pstream = NULL;
    c->pdispatch = NULL;
    c->streams = pa_dynarray_new();
    assert(c->streams);
    c->first_stream = NULL;
    c->errno = PA_ERROR_OK;
    c->state = CONTEXT_UNCONNECTED;
    c->ctag = 0;

    c->connect_complete_callback = NULL;
    c->connect_complete_userdata = NULL;

    c->die_callback = NULL;
    c->die_userdata = NULL;
    
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
    if (c->streams)
        pa_dynarray_free(c->streams, NULL, NULL);
        
    free(c->name);
    free(c);
}

static void stream_dead(struct pa_stream *s) {
    if (s->state == STREAM_DEAD)
        return;

    s->state = STREAM_DEAD;
    if (s->die_callback)
        s->die_callback(s, s->die_userdata);
}

static void context_dead(struct pa_context *c) {
    struct pa_stream *s;
    assert(c);
    
    for (s = c->first_stream; s; s = s->next)
        stream_dead(s);

    if (c->state == CONTEXT_DEAD)
        return;
    
    c->state = CONTEXT_DEAD;
    if (c->die_callback)
        c->die_callback(c, c->die_userdata);
}

static void pstream_die_callback(struct pa_pstream *p, void *userdata) {
    struct pa_context *c = userdata;
    assert(p && c);

    assert(c->state != CONTEXT_DEAD);
    
    c->state = CONTEXT_DEAD;

    context_dead(c);
}

static int pstream_packet_callback(struct pa_pstream *p, struct pa_packet *packet, void *userdata) {
    struct pa_context *c = userdata;
    assert(p && packet && c);

    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        fprintf(stderr, "polyp.c: invalid packet.\n");
        return -1;
    }

    return 0;
}

static int pstream_memblock_callback(struct pa_pstream *p, uint32_t channel, int32_t delta, struct pa_memchunk *chunk, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_stream *s;
    assert(p && chunk && c && chunk->memblock && chunk->memblock->data);

    if (!(s = pa_dynarray_get(c->streams, channel)))
        return -1;

    if (s->read_callback)
        s->read_callback(s, chunk->memblock->data + chunk->index, chunk->length, s->read_userdata);
    
    return 0;
}

static void on_connection(struct pa_socket_client *client, struct pa_iochannel*io, void *userdata) {
    struct pa_context *c = userdata;
    assert(client && io && c && c->state == CONTEXT_CONNECTING);

    pa_socket_client_free(client);
    c->client = NULL;

    if (!io) {
        c->errno = PA_ERROR_CONNECTIONREFUSED;
        c->state = CONTEXT_UNCONNECTED;

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

    c->state = CONTEXT_READY;

    if (c->connect_complete_callback)
        c->connect_complete_callback(c, 1, c->connect_complete_userdata);
}

int pa_context_connect(struct pa_context *c, const char *server, void (*complete) (struct pa_context*c, int success, void *userdata), void *userdata) {
    assert(c && c->state == CONTEXT_UNCONNECTED);

    assert(!c->client);
    if (!(c->client = pa_socket_client_new_unix(c->mainloop, server ? server : DEFAULT_SERVER))) {
        c->errno = PA_ERROR_CONNECTIONREFUSED;
        return -1;
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
    return c->errno;
}

void pa_context_set_die_callback(struct pa_context *c, void (*cb)(struct pa_context *c, void *userdata), void *userdata) {
    assert(c);
    c->die_callback = cb;
    c->die_userdata = userdata;
}

static int command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s;
    struct pa_context *c = userdata;
    uint32_t bytes, channel;
    assert(pd && command == PA_COMMAND_REQUEST && t && c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        c->errno = PA_ERROR_PROTOCOL;
        return -1;
    }
    
    if (!(s = pa_dynarray_get(c->streams, channel))) {
        c->errno = PA_ERROR_PROTOCOL;
        return -1;
    }

    /*fprintf(stderr, "Requested %u bytes\n", bytes);*/
    
    s->requested_bytes += bytes;
    
    if (s->requested_bytes && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

    return 0;
}

static int create_playback_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    int ret = 0;
    struct pa_stream *s = userdata;
    assert(pd && s && s->state == STREAM_CREATING);

    if (command != PA_COMMAND_REPLY) {
        struct pa_context *c = s->context;
        assert(c);

        if (command == PA_COMMAND_ERROR && pa_tagstruct_getu32(t, &s->context->errno) < 0) {
            s->context->errno = PA_ERROR_PROTOCOL;
            ret = -1;
        } else if (command == PA_COMMAND_TIMEOUT) {
            s->context->errno = PA_ERROR_TIMEOUT;
            ret = -1;
        }

        goto fail;
    }

    if (pa_tagstruct_getu32(t, &s->channel) < 0 ||
        pa_tagstruct_getu32(t, &s->device_index) < 0 ||
        !pa_tagstruct_eof(t)) {
        s->context->errno = PA_ERROR_PROTOCOL;
        ret = -1;
        goto fail;
    }

    s->channel_valid = 1;
    pa_dynarray_put(s->context->streams, s->channel, s);
    
    s->state = STREAM_READY;
    assert(s->create_complete_callback);
    s->create_complete_callback(s->context, s, s->create_complete_userdata);
    return 0;

fail:
    assert(s->create_complete_callback);
    s->create_complete_callback(s->context, NULL, s->create_complete_userdata);
    pa_stream_free(s);
    return ret;
}

int pa_stream_new(
    struct pa_context *c,
    enum pa_stream_direction dir,
    const char *dev,
    const char *name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr,
    void (*complete) (struct pa_context*c, struct pa_stream *s, void *userdata),
    void *userdata) {
    
    struct pa_stream *s;
    struct pa_tagstruct *t;
    uint32_t tag;

    assert(c && name && ss && c->state == CONTEXT_READY && complete);
    
    s = malloc(sizeof(struct pa_stream));
    assert(s);
    s->context = c;

    s->read_callback = NULL;
    s->read_userdata = NULL;
    s->write_callback = NULL;
    s->write_userdata = NULL;
    s->die_callback = NULL;
    s->die_userdata = NULL;
    s->create_complete_callback = complete;
    s->create_complete_userdata = NULL;

    s->state = STREAM_CREATING;
    s->requested_bytes = 0;
    s->channel = 0;
    s->channel_valid = 0;
    s->device_index = (uint32_t) -1;
    s->direction = dir;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    
    pa_tagstruct_putu32(t, dir == PA_STREAM_PLAYBACK ? PA_COMMAND_CREATE_PLAYBACK_STREAM : PA_COMMAND_CREATE_RECORD_STREAM);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_sample_spec(t, ss);
    pa_tagstruct_putu32(t, (uint32_t) -1);
    pa_tagstruct_putu32(t, attr ? attr->queue_length : DEFAULT_QUEUE_LENGTH);
    pa_tagstruct_putu32(t, attr ? attr->max_length  : DEFAULT_MAX_LENGTH);
    pa_tagstruct_putu32(t, attr ? attr->prebuf : DEFAULT_PREBUF);

    pa_pstream_send_tagstruct(c->pstream, t);

    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, create_playback_callback, s);
    
    s->next = c->first_stream;
    if (s->next)
        s->next->previous = s;
    s->previous = NULL;
    c->first_stream = s;

    return 0;
}

void pa_stream_free(struct pa_stream *s) {
    assert(s && s->context);
    
    if (s->channel_valid) {
        struct pa_tagstruct *t = pa_tagstruct_new(NULL, 0);
        assert(t);
    
        pa_tagstruct_putu32(t, PA_COMMAND_DELETE_PLAYBACK_STREAM);
        pa_tagstruct_putu32(t, s->context->ctag++);
        pa_tagstruct_putu32(t, s->channel);
        pa_pstream_send_tagstruct(s->context->pstream, t);
    }
    
    if (s->channel_valid)
        pa_dynarray_put(s->context->streams, s->channel, NULL);

    if (s->next)
        s->next->previous = s->previous;
    if (s->previous)
        s->previous->next = s->next;
    else
        s->context->first_stream = s->next;
    
    free(s);
}

void pa_stream_set_write_callback(struct pa_stream *s, void (*cb)(struct pa_stream *p, size_t length, void *userdata), void *userdata) {
    assert(s && cb);
    s->write_callback = cb;
    s->write_userdata = userdata;
}

void pa_stream_write(struct pa_stream *s, const void *data, size_t length) {
    struct pa_memchunk chunk;
    assert(s && s->context && data && length);

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
    assert(s);
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
