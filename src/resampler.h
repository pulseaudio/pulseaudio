#ifndef fooresamplerhfoo
#define fooresamplerhfoo

#include "sample.h"
#include "memblock.h"
#include "memchunk.h"

struct pa_resampler;

struct pa_resampler* pa_resampler_new(const struct pa_sample_spec *a, const struct pa_sample_spec *b);
void pa_resampler_free(struct pa_resampler *r);

size_t pa_resampler_request(struct pa_resampler *r, size_t out_length);
void pa_resampler_run(struct pa_resampler *r, const struct pa_memchunk *in, struct pa_memchunk *out);

#endif
