#ifndef fooprotocolesoundhfoo
#define fooprotocolesoundhfoo

#include "core.h"
#include "socket-server.h"

struct protocol_esound;

struct protocol_esound* protocol_esound_new(struct core*core, struct socket_server *server);
void protocol_esound_free(struct protocol_esound *p);

#endif
