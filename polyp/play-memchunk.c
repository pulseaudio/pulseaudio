#include <stdlib.h>
#include <assert.h>

#include "play-memchunk.h"
#include "sink-input.h"
#include "xmalloc.h"

static void sink_input_kill(struct pa_sink_input *i) {
    struct pa_memchunk *c;
    assert(i && i->userdata);
    c = i->userdata;

    pa_memblock_unref(c->memblock);
    pa_xfree(c);
    pa_sink_input_free(i);
}

static int sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    struct pa_memchunk *c;
    assert(i && chunk && i->userdata);
    c = i->userdata;

    if (c->length <= 0)
        return -1;
    
    assert(c->memblock && c->memblock->length);
    *chunk = *c;
    pa_memblock_ref(c->memblock);

    return 0;
}

static void si_kill(void *i) {
    sink_input_kill(i);
}

static void sink_input_drop(struct pa_sink_input *i, size_t length) {
    struct pa_memchunk *c;
    assert(i && length && i->userdata);
    c = i->userdata;

    assert(length <= c->length);

    c->length -= length;
    c->index += length;

    if (c->length <= 0)
        pa_mainloop_api_once(i->sink->core->mainloop, si_kill, i);
}

int pa_play_memchunk(struct pa_sink *sink, const char *name, const struct pa_sample_spec *ss, const struct pa_memchunk *chunk, uint32_t volume) {
    struct pa_sink_input *si;
    struct pa_memchunk *nchunk;

    assert(sink && chunk);

    if (volume <= 0)
        return 0;

    if (!(si = pa_sink_input_new(sink, name, ss)))
        return -1;

    si->volume = volume;
    si->peek = sink_input_peek;
    si->drop = sink_input_drop;
    si->kill = sink_input_kill;
    
    si->userdata = nchunk = pa_xmalloc(sizeof(struct pa_memchunk));
    *nchunk = *chunk;
    
    pa_memblock_ref(chunk->memblock);

    pa_sink_notify(sink);
    
    return 0;
}
