#include "protocol-native.h"

struct protocol_native {
    struct socket_server*server;
    struct idxset *connection;
};

struct stream_info {
    guint32_t tag;
    
    union {
        struct output_stream *output_stream;
        struct input_stream *input_stream;
    }
};

struct connection {
    struct client *client;
    struct serializer *serializer;

    
};

static void on_connection(struct socket_server *server, struct iochannel *io, void *userdata) {
    struct protocol_native *p = userdata;
    assert(server && io && p && p->server == server);

    
}

struct protocol_native* protocol_native(struct socket_server *server) {
    struct protocol_native *p;
    assert(server);

    p = malloc(sizeof(struct protocol_native));
    assert(p);

    p->server = server;
    socket_server_set_callback(p->server, callback, p);

    return p;
}

void protocol_native_free(struct protocol_native *p) {
    assert(p);

    socket_server_free(p->server);
    free(p);
}
