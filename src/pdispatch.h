#ifndef foopdispatchhfoo
#define foopdispatchhfoo

#include <inttypes.h>
#include "tagstruct.h"
#include "packet.h"
#include "mainloop-api.h"

struct pa_pdispatch;

struct pa_pdispatch_command {
    int (*proc)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
};

struct pa_pdispatch* pa_pdispatch_new(struct pa_mainloop_api *m, const struct pa_pdispatch_command*table, unsigned entries);
void pa_pdispatch_free(struct pa_pdispatch *pd);

int pa_pdispatch_run(struct pa_pdispatch *pd, struct pa_packet*p, void *userdata);

void pa_pdispatch_register_reply(struct pa_pdispatch *pd, uint32_t tag, int timeout, int (*cb)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void *userdata);

#endif
