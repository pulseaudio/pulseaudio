#ifndef fooprotocolsimplehfoo
#define fooprotocolsimplehfoo

#include "socket-server.h"

struct protocol_simple;

enum protocol_simple_mode {
    PROTOCOL_SIMPLE_RECORD = 1,
    PROTOCOL_SIMPLE_PLAYBACK = 2,
    PROTOCOL_SIMPLE_DUPLEX = 3
};

struct protocol_simple* protocol_simple_new(struct core *core, struct socket_server *server, enum protocol_simple_mode mode);
void protocol_simple_free(struct protocol_simple *n);

#endif
