#ifndef fooiochannelhfoo
#define fooiochannelhfoo

#include <sys/types.h>
#include "mainloop-api.h"

struct iochannel;

struct iochannel* iochannel_new(struct pa_mainloop_api*m, int ifd, int ofd);
void iochannel_free(struct iochannel*io);

ssize_t iochannel_write(struct iochannel*io, const void*data, size_t l);
ssize_t iochannel_read(struct iochannel*io, void*data, size_t l);

int iochannel_is_readable(struct iochannel*io);
int iochannel_is_writable(struct iochannel*io);

void iochannel_set_noclose(struct iochannel*io, int b);

void iochannel_set_callback(struct iochannel*io, void (*callback)(struct iochannel*io, void *userdata), void *userdata);

void iochannel_peer_to_string(struct iochannel*io, char*s, size_t l);

#endif
