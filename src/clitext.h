#ifndef fooclitexthfoo
#define fooclitexthfoo

#include "core.h"

char *pa_sink_input_list_to_string(struct pa_core *c);
char *pa_source_output_list_to_string(struct pa_core *c);
char *pa_sink_list_to_string(struct pa_core *core);
char *pa_source_list_to_string(struct pa_core *c);
char *pa_client_list_to_string(struct pa_core *c);
char *pa_module_list_to_string(struct pa_core *c);

#endif

