#ifndef fooiolinehfoo
#define fooiolinehfoo

#include "iochannel.h"

struct ioline;

struct ioline* ioline_new(struct iochannel *io);
void ioline_free(struct ioline *l);

void ioline_puts(struct ioline *s, const char *c);
void ioline_set_callback(struct ioline*io, void (*callback)(struct ioline*io, const char *s, void *userdata), void *userdata);

#endif
