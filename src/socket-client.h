#ifndef foosocketclienthfoo
#define foosocketclienthfoo

#include <inttypes.h>
#include "mainloop-api.h"
#include "iochannel.h"

struct pa_socket_client;

struct pa_socket_client* pa_socket_client_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port);
struct pa_socket_client* pa_socket_client_new_unix(struct pa_mainloop_api *m, const char *filename);

void pa_socket_client_free(struct pa_socket_client *c);

void pa_socket_client_set_callback(struct pa_socket_client *c, void (*on_connection)(struct pa_socket_client *c, struct pa_iochannel*io, void *userdata), void *userdata);

#endif
