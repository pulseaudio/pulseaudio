#ifndef fooprotocolclihfoo
#define fooprotocolclihfoo

#include "core.h"
#include "socket-server.h"
#include "module.h"
#include "modargs.h"

struct pa_protocol_cli;

struct pa_protocol_cli* pa_protocol_cli_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma);
void pa_protocol_cli_free(struct pa_protocol_cli *n);

#endif
