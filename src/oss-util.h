#ifndef fooossutilhfoo
#define fooossutilhfoo

#include "sample.h"

int pa_oss_open(const char *device, int *mode, int* pcaps);
int pa_oss_auto_format(int fd, struct pa_sample_spec *ss);

int pa_oss_set_fragments(int fd, int frags, int frag_size);

#endif
