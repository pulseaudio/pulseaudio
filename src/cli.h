#ifndef fooclihfoo
#define fooclihfoo

#include "iochannel.h"
#include "core.h"

struct cli;

struct cli* cli_new(struct core *core, struct iochannel *io);
void cli_free(struct cli *cli);

void cli_set_eof_callback(struct cli *cli, void (*cb)(struct cli*c, void *userdata), void *userdata);

#endif
