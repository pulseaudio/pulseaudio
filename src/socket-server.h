#ifndef foosocketserverhfoo
#define foosocketserverhfoo

#include <inttypes.h>
#include "mainloop-api.h"
#include "iochannel.h"

struct pa_socket_server;

struct pa_socket_server* pa_socket_server_new(struct pa_mainloop_api *m, int fd);
struct pa_socket_server* pa_socket_server_new_unix(struct pa_mainloop_api *m, const char *filename);
struct pa_socket_server* pa_socket_server_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port);

void pa_socket_server_free(struct pa_socket_server*s);

void pa_socket_server_set_callback(struct pa_socket_server*s, void (*on_connection)(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata), void *userdata);

#endif
