#ifndef foosimplehfoo
#define foosimplehfoo

#include <sys/types.h>

#include "sample.h"
#include "polypdef.h"

struct pa_simple;

struct pa_simple* pa_simple_new(
    const char *server,
    const char *name,
    enum pa_stream_direction dir,
    const char *dev,
    const char *stream_name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr,
    int *error);

void pa_simple_free(struct pa_simple *s);

int pa_simple_write(struct pa_simple *s, const void*data, size_t length, int *error);
int pa_simple_read(struct pa_simple *s, void*data, size_t length, int *error);

#endif
