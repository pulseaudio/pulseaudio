#ifndef foosocketserverhfoo
#define foosocketserverhfoo

#include <inttypes.h>
#include "mainloop-api.h"
#include "iochannel.h"

struct socket_server;

struct socket_server* socket_server_new(struct pa_mainloop_api *m, int fd);
struct socket_server* socket_server_new_unix(struct pa_mainloop_api *m, const char *filename);
struct socket_server* socket_server_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port);

void socket_server_free(struct socket_server*s);

void socket_server_set_callback(struct socket_server*s, void (*on_connection)(struct socket_server*s, struct iochannel *io, void *userdata), void *userdata);

#endif
