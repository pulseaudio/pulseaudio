#ifndef fooprotocolnativehfoo
#define fooprotocolnativehfoo

#include "core.h"
#include "socket-server.h"
#include "module.h"
#include "modargs.h"

struct pa_protocol_native;

struct pa_protocol_native* pa_protocol_native_new(struct pa_core*core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma);
void pa_protocol_native_free(struct pa_protocol_native *n);

#endif
