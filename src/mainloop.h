#ifndef foomainloophfoo
#define foomainloophfoo

#include "mainloop-api.h"

struct pa_mainloop;

struct pa_mainloop *pa_mainloop_new(void);
void pa_mainloop_free(struct pa_mainloop* m);

int pa_mainloop_iterate(struct pa_mainloop *m, int block, int *retval);
int pa_mainloop_run(struct pa_mainloop *m, int *retval);

struct pa_mainloop_api* pa_mainloop_get_api(struct pa_mainloop*m);

#endif
