#ifndef foopdispatchhfoo
#define foopdispatchhfoo

#include <inttypes.h>
#include "tagstruct.h"
#include "packet.h"
#include "mainloop-api.h"

struct pdispatch;

struct pdispatch_command {
    int (*proc)(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata);
};

struct pdispatch* pdispatch_new(struct pa_mainloop_api *m, const struct pdispatch_command*table, unsigned entries);
void pdispatch_free(struct pdispatch *pd);

int pdispatch_run(struct pdispatch *pd, struct packet*p, void *userdata);

void pdispatch_register_reply(struct pdispatch *pd, uint32_t tag, int timeout, int (*cb)(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata), void *userdata);

#endif
