#ifndef foopolypdefhfoo
#define foopolypdefhfoo

#include <inttypes.h>

enum pa_stream_direction {
    PA_STREAM_PLAYBACK,
    PA_STREAM_RECORD
};

struct pa_buffer_attr {
    uint32_t queue_length;
    uint32_t max_length;
    uint32_t prebuf;
};


#endif
