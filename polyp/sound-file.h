#ifndef soundfilehfoo
#define soundfilehfoo

#include "memchunk.h"
#include "sample.h"

int pa_sound_file_load(const char *fname, struct pa_sample_spec *ss, struct pa_memchunk *chunk);

#endif
