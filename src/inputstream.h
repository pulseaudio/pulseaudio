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

    void (*kill)(struct input_stream* i, void *userdata);
    void *kill_userdata;
};

struct input_stream* input_stream_new(struct sink *s, struct sample_spec *spec, const char *name);
void input_stream_free(struct input_stream* i);

/* This function notifies the attached sink that new data is available
 * in the memblockq */
void input_stream_notify_sink(struct input_stream *i);


/* The registrant of the input stream should call this function to set a
 * callback function which is called when destruction of the input stream is
 * requested */
void input_stream_set_kill_callback(struct input_stream *c, void (*kill)(struct input_stream*i, void *userdata), void *userdata);

/* Code that didn't create the input stream should call this function to
 * request destruction of it */
void input_stream_kill(struct input_stream *c);

#endif
