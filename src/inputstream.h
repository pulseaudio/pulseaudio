#ifndef fooinputstreamhfoo
#define fooinputstreamhfoo

#include <inttypes.h>

#include "sink.h"
#include "sample.h"
#include "memblockq.h"

struct input_stream {
    char *name;
    uint32_t index;

    struct sink *sink;
    struct sample_spec spec;
    
    struct memblockq *memblockq;
};

struct input_stream* input_stream_new(struct sink *s, struct sample_spec *spec, const char *name);
void input_stream_free(struct input_stream* i);

void input_stream_notify(struct input_stream *i);

#endif
