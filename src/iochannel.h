#ifndef fooiochannelhfoo
#define fooiochannelhfoo

#include <sys/types.h>
#include "mainloop-api.h"

/* It is safe to destroy the calling iochannel object from the callback */

struct pa_iochannel;

struct pa_iochannel* pa_iochannel_new(struct pa_mainloop_api*m, int ifd, int ofd);
void pa_iochannel_free(struct pa_iochannel*io);

ssize_t pa_iochannel_write(struct pa_iochannel*io, const void*data, size_t l);
ssize_t pa_iochannel_read(struct pa_iochannel*io, void*data, size_t l);

int pa_iochannel_is_readable(struct pa_iochannel*io);
int pa_iochannel_is_writable(struct pa_iochannel*io);
int pa_iochannel_is_hungup(struct pa_iochannel*io);

void pa_iochannel_set_noclose(struct pa_iochannel*io, int b);

void pa_iochannel_set_callback(struct pa_iochannel*io, void (*callback)(struct pa_iochannel*io, void *userdata), void *userdata);

void pa_iochannel_socket_peer_to_string(struct pa_iochannel*io, char*s, size_t l);
int pa_iochannel_socket_set_rcvbuf(struct pa_iochannel*io, size_t l);
int pa_iochannel_socket_set_sndbuf(struct pa_iochannel*io, size_t l);

#endif
