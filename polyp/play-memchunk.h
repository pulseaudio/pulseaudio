#ifndef fooplaychunkhfoo
#define fooplaychunkhfoo

#include "sink.h"

int pa_play_memchunk(struct pa_sink *sink, const char *name, const struct pa_sample_spec *ss, const struct pa_memchunk *chunk, uint32_t volume);

#endif
