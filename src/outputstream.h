#ifndef foooutputstreamhfoo
#define foooutputstreamhfoo

#include <inttypes.h>
#include "source.h"
#include "sample.h"
#include "memblockq.h"

struct output_stream {
    char *name;
    uint32_t index;

    struct source *source;
    struct sample_spec spec;
    
    struct memblockq *memblockq;
    void (*kill)(struct output_stream* i, void *userdata);
    void *kill_userdata;
};

struct output_stream* output_stream_new(struct source *s, struct sample_spec *spec, const char *name);
void output_stream_free(struct output_stream* o);

void output_stream_set_kill_callback(struct output_stream *i, void (*kill)(struct output_stream*i, void *userdata), void *userdata);
void output_stream_kill(struct output_stream*i);

#endif
