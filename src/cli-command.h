#ifndef fooclicommandhfoo
#define fooclicommandhfoo

#include "strbuf.h"
#include "core.h"

int pa_cli_command_execute_line(struct pa_core *c, const char *s, struct pa_strbuf *buf, int *fail, int *verbose);
int pa_cli_command_execute_file(struct pa_core *c, const char *fn, struct pa_strbuf *buf, int *fail, int *verbose);
int pa_cli_command_execute(struct pa_core *c, const char *s, struct pa_strbuf *buf, int *fail, int *verbose);

#endif
