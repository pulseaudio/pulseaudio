#ifndef foosocketclienthfoo
#define foosocketclienthfoo

#include <inttypes.h>
#include "mainloop-api.h"
#include "iochannel.h"

struct socket_client;

struct socket_client* socket_client_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port);
struct socket_client* socket_client_new_unix(struct pa_mainloop_api *m, const char *filename);

void socket_client_free(struct socket_client *c);

void socket_client_set_callback(struct socket_client *c, void (*on_connection)(struct socket_client *c, struct iochannel*io, void *userdata), void *userdata);

#endif
