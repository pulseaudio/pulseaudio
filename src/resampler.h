#ifndef fooresamplerhfoo
#define fooresamplerhfoo

#include "sample.h"
#include "memblock.h"

struct resampler;

struct resampler* resampler_new(const struct pa_sample_spec *a, const struct pa_sample_spec *b);
void resampler_free(struct resampler *r);

size_t resampler_request(struct resampler *r, size_t out_length);
int resampler_run(struct resampler *r, struct memchunk *in, struct memchunk *out);

#endif
