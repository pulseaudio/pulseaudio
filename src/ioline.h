#ifndef fooiolinehfoo
#define fooiolinehfoo

#include "iochannel.h"

struct pa_ioline;

struct pa_ioline* pa_ioline_new(struct pa_iochannel *io);
void pa_ioline_free(struct pa_ioline *l);

void pa_ioline_puts(struct pa_ioline *s, const char *c);
void pa_ioline_set_callback(struct pa_ioline*io, void (*callback)(struct pa_ioline*io, const char *s, void *userdata), void *userdata);

#endif
