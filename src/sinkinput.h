#ifndef foosinkinputhfoo
#define foosinkinputhfoo

#include <inttypes.h>

#include "sink.h"
#include "sample.h"
#include "memblockq.h"

struct sink_input {
    uint32_t index;

    char *name;
    struct sink *sink;
    struct sample_spec spec;
    
    int (*peek) (struct sink_input *i, struct memchunk *chunk, uint8_t *volume);
    void (*drop) (struct sink_input *i, size_t length);
    void (*kill) (struct sink_input *i);

    void *userdata;
};

struct sink_input* sink_input_new(struct sink *s, struct sample_spec *spec, const char *name);
void sink_input_free(struct sink_input* i);

/* Code that didn't create the input stream should call this function to
 * request destruction of it */
void sink_input_kill(struct sink_input *i);


#endif
