#ifndef fooprotocolclihfoo
#define fooprotocolclihfoo

#include "core.h"
#include "socket-server.h"

struct protocol_cli;

struct protocol_cli* protocol_cli_new(struct core *core, struct socket_server *server);
void protocol_cli_free(struct protocol_cli *n);

#endif
