#ifndef foopolyplibdefhfoo
#define foopolyplibdefhfoo

#include <inttypes.h>

enum pa_stream_direction {
    PA_STREAM_PLAYBACK,
    PA_STREAM_RECORD
};

struct pa_buffer_attr {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
};

#endif
