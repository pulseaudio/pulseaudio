#ifndef fooclihfoo
#define fooclihfoo

#include "iochannel.h"
#include "core.h"
#include "module.h"

struct pa_cli;

struct pa_cli* pa_cli_new(struct pa_core *core, struct pa_iochannel *io, struct pa_module *m);
void pa_cli_free(struct pa_cli *cli);

void pa_cli_set_eof_callback(struct pa_cli *cli, void (*cb)(struct pa_cli*c, void *userdata), void *userdata);

#endif
