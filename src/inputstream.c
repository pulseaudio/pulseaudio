#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "inputstream.h"

struct input_stream* input_stream_new(struct sink *s, struct sample_spec *spec, const char *name) {
    struct input_stream *i;
    int r;
    assert(s && spec);

    i = malloc(sizeof(struct input_stream));
    assert(i);
    i->name = name ? strdup(name) : NULL;
    i->sink = s;
    i->spec = *spec;

    i->kill = NULL;
    i->kill_userdata = NULL;
    i->notify = NULL;
    i->notify_userdata = NULL;

    i->memblockq = memblockq_new(bytes_per_second(spec)*5, sample_size(spec), (size_t) -1);
    assert(i->memblockq);
    
    assert(s->core);
    r = idxset_put(s->core->input_streams, i, &i->index);
    assert(r == 0 && i->index != IDXSET_INVALID);
    r = idxset_put(s->input_streams, i, NULL);
    assert(r == 0);
    
    return i;    
}

void input_stream_free(struct input_stream* i) {
    assert(i);

    memblockq_free(i->memblockq);

    assert(i->sink && i->sink->core);
    idxset_remove_by_data(i->sink->core->input_streams, i, NULL);
    idxset_remove_by_data(i->sink->input_streams, i, NULL);
    
    free(i->name);
    free(i);
}

void input_stream_notify_sink(struct input_stream *i) {
    assert(i);

    if (!memblockq_is_readable(i->memblockq))
        return;
    
    sink_notify(i->sink);
}

void input_stream_set_kill_callback(struct input_stream *i, void (*kill)(struct input_stream*i, void *userdata), void *userdata) {
    assert(i && kill);
    i->kill = kill;
    i->kill_userdata = userdata;
}


void input_stream_kill(struct input_stream*i) {
    assert(i);

    if (i->kill)
        i->kill(i, i->kill_userdata);
}

void input_stream_set_notify_callback(struct input_stream *i, void (*notify)(struct input_stream*i, void *userdata), void *userdata) {
    assert(i && notify);

    i->notify = notify;
    i->notify_userdata = userdata;
}

void input_stream_notify(struct input_stream *i) {
    assert(i);
    if (i->notify)
        i->notify(i, i->notify_userdata);
}
