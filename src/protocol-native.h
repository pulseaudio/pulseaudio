#ifndef fooprotocolnativehfoo
#define fooprotocolnativehfoo

#include "core.h"
#include "socket-server.h"

struct protocol_native;

struct protocol_native* protocol_native_new(struct core*core, struct socket_server *server);
void protocol_native_free(struct protocol_native *n);

#endif
