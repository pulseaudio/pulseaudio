#ifndef fooprotocolesoundhfoo
#define fooprotocolesoundhfoo

#include "core.h"
#include "socket-server.h"

struct pa_protocol_esound;

struct pa_protocol_esound* pa_protocol_esound_new(struct pa_core*core, struct pa_socket_server *server);
void pa_protocol_esound_free(struct pa_protocol_esound *p);

#endif
