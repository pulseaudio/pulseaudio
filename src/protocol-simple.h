#ifndef fooprotocolsimplehfoo
#define fooprotocolsimplehfoo

#include "socket-server.h"
#include "module.h"
#include "core.h"
#include "modargs.h"

struct pa_protocol_simple;

struct pa_protocol_simple* pa_protocol_simple_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma);
void pa_protocol_simple_free(struct pa_protocol_simple *n);

#endif
