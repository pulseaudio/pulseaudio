#ifndef foomainloopsignalhfoo
#define foomainloopsignalhfoo

#include "mainloop-api.h"

int pa_signal_init(struct pa_mainloop_api *api);
void pa_signal_done(void);

void* pa_signal_register(int signal, void (*callback) (void *id, int signal, void *userdata), void *userdata);
void pa_signal_unregister(void *id);

#endif
