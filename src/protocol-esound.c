#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <limits.h>

#include "protocol-esound.h"
#include "esound-spec.h"
#include "memblock.h"
#include "client.h"
#include "sinkinput.h"
#include "sink.h"
#include "sample.h"

/* This is heavily based on esound's code */

struct connection {
    uint32_t index;
    struct protocol_esound *protocol;
    struct iochannel *io;
    struct client *client;
    int authorized, swap_byte_order;
    void *read_data;
    size_t read_data_alloc, read_data_length;
    void *write_data;
    size_t write_data_alloc, write_data_index, write_data_length;
    esd_proto_t request;
    esd_client_state_t state;
    struct sink_input *sink_input;
    struct memblockq *input_memblockq;
};

struct protocol_esound {
    int public;
    struct core *core;
    struct socket_server *server;
    struct idxset *connections;
    uint32_t sink_index;
    unsigned n_player;
};

typedef struct proto_handler {
    size_t data_length;
    int (*proc)(struct connection *c, const void *data, size_t length);
    const char *description;
} esd_proto_handler_info_t;

#define MEMBLOCKQ_LENGTH (10*1204)
#define MEMBLOCKQ_PREBUF (2*1024)

#define BUFSIZE (1024)

static void sink_input_drop_cb(struct sink_input *i, size_t length);
static int sink_input_peek_cb(struct sink_input *i, struct memchunk *chunk);
static void sink_input_kill_cb(struct sink_input *i);
static uint32_t sink_input_get_latency_cb(struct sink_input *i);

static int esd_proto_connect(struct connection *c, const void *data, size_t length);
static int esd_proto_stream_play(struct connection *c, const void *data, size_t length);
static int esd_proto_stream_record(struct connection *c, const void *data, size_t length);
static int esd_proto_get_latency(struct connection *c, const void *data, size_t length);
static int esd_proto_server_info(struct connection *c, const void *data, size_t length);
static int esd_proto_all_info(struct connection *c, const void *data, size_t length);
static int esd_proto_stream_pan(struct connection *c, const void *data, size_t length);

static int do_write(struct connection *c);

/* the big map of protocol handler info */
static struct proto_handler proto_map[ESD_PROTO_MAX] = {
    { ESD_KEY_LEN + sizeof(int),      esd_proto_connect, "connect" },
    { ESD_KEY_LEN + sizeof(int),      NULL, "lock" },
    { ESD_KEY_LEN + sizeof(int),      NULL, "unlock" },

    { ESD_NAME_MAX + 2 * sizeof(int), esd_proto_stream_play, "stream play" },
    { ESD_NAME_MAX + 2 * sizeof(int), esd_proto_stream_record, "stream rec" },
    { ESD_NAME_MAX + 2 * sizeof(int), NULL, "stream mon" },

    { ESD_NAME_MAX + 3 * sizeof(int), NULL, "sample cache" },
    { sizeof(int),                    NULL, "sample free" },
    { sizeof(int),                    NULL, "sample play" },
    { sizeof(int),                    NULL, "sample loop" },
    { sizeof(int),                    NULL, "sample stop" },
    { -1,                             NULL, "TODO: sample kill" },

    { ESD_KEY_LEN + sizeof(int),      NULL, "standby" },
    { ESD_KEY_LEN + sizeof(int),      NULL, "resume" },

    { ESD_NAME_MAX,                   NULL, "sample getid" },
    { ESD_NAME_MAX + 2 * sizeof(int), NULL, "stream filter" },

    { sizeof(int),                    esd_proto_server_info, "server info" },
    { sizeof(int),                    esd_proto_all_info, "all info" },
    { -1,                             NULL, "TODO: subscribe" },
    { -1,                             NULL, "TODO: unsubscribe" },

    { 3 * sizeof(int),                esd_proto_stream_pan, "stream pan"},
    { 3 * sizeof(int),                NULL, "sample pan" },
     
    { sizeof(int),                    NULL, "standby mode" },
    { 0,                              esd_proto_get_latency, "get latency" }
};


static void connection_free(struct connection *c) {
    assert(c);
    idxset_remove_by_data(c->protocol->connections, c, NULL);

    if (c->state == ESD_STREAMING_DATA)
        c->protocol->n_player--;
    
    client_free(c->client);
    
    if (c->sink_input)
        sink_input_free(c->sink_input);
    if (c->input_memblockq)
        memblockq_free(c->input_memblockq);
    
    free(c->read_data);
    free(c->write_data);
    
    iochannel_free(c->io);
    free(c);
}

static struct sink* get_output_sink(struct protocol_esound *p) {
    struct sink *s;
    assert(p);

    if (!(s = idxset_get_by_index(p->core->sinks, p->sink_index)))
        s = sink_get_default(p->core);

    if (s->index)
        p->sink_index = s->index;
    else
        p->sink_index = IDXSET_INVALID;

    return s;
}

static void* connection_write(struct connection *c, size_t length) {
    size_t t, i;
    assert(c);

    t = c->write_data_length+length;
    
    if (c->write_data_alloc < t)
        c->write_data = realloc(c->write_data, c->write_data_alloc = t);

    assert(c->write_data);

    i = c->write_data_length;
    c->write_data_length += length;
    
    return c->write_data+i;
}

/*** esound commands ***/

static int esd_proto_connect(struct connection *c, const void *data, size_t length) {
    uint32_t ekey;
    int *ok;
    assert(length == (ESD_KEY_LEN + sizeof(uint32_t)));

    c->authorized = 1;
    
    ekey = *(uint32_t*)(data+ESD_KEY_LEN);
    if (ekey == ESD_ENDIAN_KEY)
        c->swap_byte_order = 0;
    else if (ekey == ESD_SWAP_ENDIAN_KEY)
        c->swap_byte_order = 1;
    else {
        fprintf(stderr, "protocol-esound.c: client sent invalid endian key\n");
        return -1;
    }

    ok = connection_write(c, sizeof(int));
    assert(ok);
    *ok = 1;
    return 0;
}

static int esd_proto_stream_play(struct connection *c, const void *data, size_t length) {
    char name[ESD_NAME_MAX];
    int format, rate;
    struct sink *sink;
    struct pa_sample_spec ss;
    assert(length == (sizeof(int)*2+ESD_NAME_MAX));
    
    format = maybe_swap_endian_32(c->swap_byte_order, *(int*)data);
    rate = maybe_swap_endian_32(c->swap_byte_order, *((int*)data + 1));

    ss.rate = rate;
    ss.channels = ((format & ESD_MASK_CHAN) == ESD_STEREO) ? 2 : 1;
    ss.format = ((format & ESD_MASK_BITS) == ESD_BITS16) ? PA_SAMPLE_S16NE : PA_SAMPLE_U8;

    if (!pa_sample_spec_valid(&ss))
        return -1;

    if (!(sink = get_output_sink(c->protocol)))
        return -1;
    
    strncpy(name, data + sizeof(int)*2, sizeof(name));
    name[sizeof(name)-1] = 0;

    client_rename(c->client, name);

    assert(!c->input_memblockq);
    c->input_memblockq = memblockq_new(MEMBLOCKQ_LENGTH, pa_sample_size(&ss), MEMBLOCKQ_PREBUF);
    assert(c->input_memblockq);

    assert(!c->sink_input);
    c->sink_input = sink_input_new(sink, &ss, name);
    assert(c->sink_input);

    c->sink_input->peek = sink_input_peek_cb;
    c->sink_input->drop = sink_input_drop_cb;
    c->sink_input->kill = sink_input_kill_cb;
    c->sink_input->get_latency = sink_input_get_latency_cb;
    c->sink_input->userdata = c;
    
    c->state = ESD_STREAMING_DATA;

    c->protocol->n_player++;
    
    return 0;
}

static int esd_proto_stream_record(struct connection *c, const void *data, size_t length) {
    assert(c && data && length == (sizeof(int)*2+ESD_NAME_MAX));

    assert(0);
}

static int esd_proto_get_latency(struct connection *c, const void *data, size_t length) {
    struct sink *sink;
    int latency, *lag;
    assert(c && !data && length == 0);

    if (!(sink = get_output_sink(c->protocol)))
        latency = 0;
    else {
        float usec = sink_get_latency(sink);
        usec += pa_samples_usec(MEMBLOCKQ_LENGTH-BUFSIZE, &sink->sample_spec);
        latency = (int) ((usec*44100)/1000000);
    }
    
    lag = connection_write(c, sizeof(int));
    assert(lag);
    *lag = c->swap_byte_order ? swap_endian_32(latency) : latency;
    return 0;
}

static int esd_proto_server_info(struct connection *c, const void *data, size_t length) {
    int rate = 44100, format = ESD_STEREO|ESD_BITS16;
    int *response;
    struct sink *sink;
    assert(c && data && length == sizeof(int));

    if ((sink = get_output_sink(c->protocol))) {
        rate = sink->sample_spec.rate;
        format = (sink->sample_spec.format == PA_SAMPLE_U8) ? ESD_BITS8 : ESD_BITS16;
        format |= (sink->sample_spec.channels >= 2) ? ESD_STEREO : ESD_MONO;
    }
    
    response = connection_write(c, sizeof(int)*3);
    assert(response);
    *(response++) = 0;
    *(response++) = maybe_swap_endian_32(c->swap_byte_order, rate);
    *(response++) = maybe_swap_endian_32(c->swap_byte_order, format);
    return 0;
}

static int esd_proto_all_info(struct connection *c, const void *data, size_t length) {
    void *response;
    size_t t, k, s;
    struct connection *conn;
    size_t index = IDXSET_INVALID;
    assert(c && data && length == sizeof(int));
    
    if (esd_proto_server_info(c, data, length) < 0)
        return -1;

    k = sizeof(int)*5+ESD_NAME_MAX;
    s = sizeof(int)*6+ESD_NAME_MAX;
    response = connection_write(c, (t = s+k*(c->protocol->n_player+1)));
    assert(k);

    for (conn = idxset_first(c->protocol->connections, &index); conn; conn = idxset_next(c->protocol->connections, &index)) {
        int format = ESD_BITS16 | ESD_STEREO, rate = 44100, volume = 0xFF;

        if (conn->state != ESD_STREAMING_DATA)
            continue;

        assert(t >= s+k+k);
        
        if (conn->sink_input) {
            rate = conn->sink_input->sample_spec.rate;
            volume = (conn->sink_input->volume*0xFF)/0x100;
            format = (conn->sink_input->sample_spec.format == PA_SAMPLE_U8) ? ESD_BITS8 : ESD_BITS16;
            format |= (conn->sink_input->sample_spec.channels >= 2) ? ESD_STEREO : ESD_MONO;
        }
        
        /* id */
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, (int) conn->index);
        response += sizeof(int);

        /* name */
        assert(conn->client);
        strncpy(response, conn->client->name, ESD_NAME_MAX);
        response += ESD_NAME_MAX;

        /* rate */
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order,  rate);
        response += sizeof(int);

        /* left */
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order,  volume);
        response += sizeof(int);

        /*right*/
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order,  volume);
        response += sizeof(int);

        /*format*/
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, format);
        response += sizeof(int);

        t-= k;
    }

    assert(t == s+k);
    memset(response, 0, t);
    return 0;
}

static int esd_proto_stream_pan(struct connection *c, const void *data, size_t length) {
    int *ok;
    uint32_t index, volume;
    struct connection *conn;
    assert(c && data && length == sizeof(int)*3);
    
    index = (uint32_t) maybe_swap_endian_32(c->swap_byte_order, *(int*)data);
    volume = (uint32_t) maybe_swap_endian_32(c->swap_byte_order, *((int*)data + 1));
    volume = (volume*0x100)/0xFF;

    ok = connection_write(c, sizeof(int));
    assert(ok);

    if ((conn = idxset_get_by_index(c->protocol->connections, index))) {
        assert(conn->sink_input);
        conn->sink_input->volume = volume;
        *ok = 1;
    } else
        *ok = 0;
    
    return 0;
}

/*** client callbacks ***/

static void client_kill_cb(struct client *c) {
    assert(c && c->userdata);
    connection_free(c->userdata);
}

/*** iochannel callbacks ***/

static int do_read(struct connection *c) {
    assert(c && c->io);

    if (!iochannel_is_readable(c->io))
        return 0;

    if (c->state == ESD_NEXT_REQUEST) {
        ssize_t r;
        assert(c->read_data_length < sizeof(c->request));

        if ((r = iochannel_read(c->io, ((void*) &c->request) + c->read_data_length, sizeof(c->request) - c->read_data_length)) <= 0) {
            fprintf(stderr, "protocol-esound.c: read() failed: %s\n", r == 0 ? "EOF" : strerror(errno));
            return -1;
        }

        if ((c->read_data_length+= r) >= sizeof(c->request)) {
            struct proto_handler *handler;
            
            if (c->swap_byte_order)
                c->request = swap_endian_32(c->request);

            if (c->request < ESD_PROTO_CONNECT || c->request > ESD_PROTO_MAX) {
                fprintf(stderr, "protocol-esound.c: recieved invalid request.\n");
                return -1;
            }

            handler = proto_map+c->request;

            if (!handler->proc) {
                fprintf(stderr, "protocol-sound.c: recieved unimplemented request.\n");
                return -1;
            }
            
            if (handler->data_length == 0) {
                c->read_data_length = 0;

                if (handler->proc(c, NULL, 0) < 0)
                    return -1;
                
            } else {
                if (c->read_data_alloc < handler->data_length)
                    c->read_data = realloc(c->read_data, c->read_data_alloc = handler->data_length);
                assert(c->read_data);
                
                c->state = ESD_NEEDS_REQDATA;
                c->read_data_length = 0;
            }
        }

    } else if (c->state == ESD_NEEDS_REQDATA) {
        ssize_t r;
        struct proto_handler *handler = proto_map+c->request;

        assert(handler->proc);
        
        assert(c->read_data && c->read_data_length < handler->data_length);

        if ((r = iochannel_read(c->io, c->read_data + c->read_data_length, handler->data_length - c->read_data_length)) <= 0) {
            fprintf(stderr, "protocol-esound.c: read() failed: %s\n", r == 0 ? "EOF" : strerror(errno));
            return -1;
        }

        if ((c->read_data_length+= r) >= handler->data_length) {
            size_t l = c->read_data_length;
            assert(handler->proc);

            c->state = ESD_NEXT_REQUEST;
            c->read_data_length = 0;
            
            if (handler->proc(c, c->read_data, l) < 0)
                return -1;
        }
    } else if (c->state == ESD_STREAMING_DATA) {
        struct memchunk chunk;
        ssize_t r;

        assert(c->input_memblockq);

        if (!memblockq_is_writable(c->input_memblockq, BUFSIZE))
            return 0;

        chunk.memblock = memblock_new(BUFSIZE);
        assert(chunk.memblock && chunk.memblock->data);

        if ((r = iochannel_read(c->io, chunk.memblock->data, BUFSIZE)) <= 0) {
            fprintf(stderr, "protocol-esound.c: read() failed: %s\n", r == 0 ? "EOF" : strerror(errno));
            memblock_unref(chunk.memblock);
            return -1;
        }

        chunk.memblock->length = chunk.length = r;
        chunk.index = 0;

        assert(c->input_memblockq);
        memblockq_push(c->input_memblockq, &chunk, 0);
        memblock_unref(chunk.memblock);
        assert(c->sink_input);
        sink_notify(c->sink_input->sink);

    } else
        assert(0);

    return 0;
}

static int do_write(struct connection *c) {
    ssize_t r;
    assert(c && c->io);

    if (!iochannel_is_writable(c->io))
        return 0;

    if (!c->write_data_length)
        return 0;

    assert(c->write_data_index < c->write_data_length);
    if ((r = iochannel_write(c->io, c->write_data+c->write_data_index, c->write_data_length-c->write_data_index)) < 0) {
        fprintf(stderr, "protocol-esound.c: write() failed: %s\n", strerror(errno));
        return -1;
    }

    if ((c->write_data_index +=r) >= c->write_data_length)
        c->write_data_length = c->write_data_index = 0;
    
    return 0;
}

static void io_callback(struct iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c && c->io == io);

    if (do_read(c) < 0 || do_write(c) < 0)
        connection_free(c);
}

/*** sink_input callbacks ***/

static int sink_input_peek_cb(struct sink_input *i, struct memchunk *chunk) {
    struct connection*c;
    assert(i && i->userdata && chunk);
    c = i->userdata;
    
    if (memblockq_peek(c->input_memblockq, chunk) < 0)
        return -1;

    return 0;
}

static void sink_input_drop_cb(struct sink_input *i, size_t length) {
    struct connection*c = i->userdata;
    assert(i && c && length);

    memblockq_drop(c->input_memblockq, length);
    
    if (do_read(c) < 0)
        connection_free(c);
}

static void sink_input_kill_cb(struct sink_input *i) {
    assert(i && i->userdata);
    connection_free((struct connection *) i->userdata);
}


static uint32_t sink_input_get_latency_cb(struct sink_input *i) {
    struct connection*c = i->userdata;
    assert(i && c);
    return pa_samples_usec(memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);
}

/*** socket server callback ***/

static void on_connection(struct socket_server*s, struct iochannel *io, void *userdata) {
    struct connection *c;
    char cname[256];
    assert(s && io && userdata);

    c = malloc(sizeof(struct connection));
    assert(c);
    c->protocol = userdata;
    c->io = io;
    iochannel_set_callback(c->io, io_callback, c);

    iochannel_peer_to_string(io, cname, sizeof(cname));
    assert(c->protocol->core);
    c->client = client_new(c->protocol->core, "ESOUND", cname);
    assert(c->client);
    c->client->kill = client_kill_cb;
    c->client->userdata = c;
    
    c->authorized = c->protocol->public;
    c->swap_byte_order = 0;

    c->read_data_length = 0;
    c->read_data = malloc(c->read_data_alloc = proto_map[ESD_PROTO_CONNECT].data_length);
    assert(c->read_data);

    c->write_data_length = c->write_data_index = c->write_data_alloc = 0;
    c->write_data = NULL;

    c->state = ESD_NEEDS_REQDATA;
    c->request = ESD_PROTO_CONNECT;

    c->sink_input = NULL;
    c->input_memblockq = NULL;

    idxset_put(c->protocol->connections, c, &c->index);
}

/*** entry points ***/

struct protocol_esound* protocol_esound_new(struct core*core, struct socket_server *server) {
    struct protocol_esound *p;
    assert(core && server);

    p = malloc(sizeof(struct protocol_esound));
    assert(p);
    p->public = 1;
    p->server = server;
    p->core = core;
    p->connections = idxset_new(NULL, NULL);
    assert(p->connections);
    p->sink_index = IDXSET_INVALID;

    p->n_player = 0;

    socket_server_set_callback(p->server, on_connection, p);

    return p;
}

void protocol_esound_free(struct protocol_esound *p) {
    struct connection *c;
    assert(p);

    while ((c = idxset_first(p->connections, NULL)))
        connection_free(c);

    idxset_free(p->connections, NULL, NULL);
    socket_server_free(p->server);
    free(p);
}
