#ifndef fooprotocolsimplehfoo
#define fooprotocolsimplehfoo

#include "socket-server.h"
#include "module.h"
#include "core.h"

struct pa_protocol_simple;

enum pa_protocol_simple_mode {
    PA_PROTOCOL_SIMPLE_RECORD = 1,
    PA_PROTOCOL_SIMPLE_PLAYBACK = 2,
    PA_PROTOCOL_SIMPLE_DUPLEX = 3
};

struct pa_protocol_simple* pa_protocol_simple_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, enum pa_protocol_simple_mode mode);
void pa_protocol_simple_free(struct pa_protocol_simple *n);

#endif
