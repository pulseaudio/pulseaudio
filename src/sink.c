#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "sink.h"
#include "inputstream.h"

struct sink* sink_new(struct core *core, const char *name, const struct sample_spec *spec) {
    struct sink *s;
    char *n = NULL;
    int r;
    assert(core && spec);

    s = malloc(sizeof(struct sink));
    assert(s);
    
    s->name = name ? strdup(name) : NULL;
    r = idxset_put(core->sinks, s, &s->index);
    assert(s->index != IDXSET_INVALID && r >= 0);

    s->core = core;
    s->sample_spec = *spec;
    s->input_streams = idxset_new(NULL, NULL);

    if (name) {
        n = malloc(strlen(name)+9);
        sprintf(n, "%s_monitor", name);
    }
    
    s->monitor_source = source_new(core, n, spec);
    free(n);
    
    s->volume = 0xFF;

    s->notify = NULL;
    s->notify_userdata = NULL;

    return s;
}

void sink_free(struct sink *s) {
    struct input_stream *i, *j = NULL;
    assert(s);

    while ((i = idxset_first(s->input_streams, NULL))) {
        assert(i != j);
        input_stream_kill(i);
        j = i;
    }
    idxset_free(s->input_streams, NULL, NULL);
        
    idxset_remove_by_data(s->core->sinks, s, NULL);
    source_free(s->monitor_source);

    free(s->name);
    free(s);
}

struct pass1_info {
    size_t maxlength;
    unsigned count;
    struct input_stream *last_input_stream;
};

static int get_max_length(void *p, uint32_t index, int *del, void*userdata) {
    struct memchunk chunk;
    struct pass1_info *info = userdata;
    struct input_stream*i = p;
    assert(info && i);

    if (memblockq_peek(i->memblockq, &chunk) != 0)
        return 0;

    assert(chunk.length);
    
    if (info->maxlength > chunk.length)
        info->maxlength = chunk.length;

    info->count++;
    info->last_input_stream = i;

    memblock_unref(chunk.memblock);

    return 0;
}

struct pass2_info {
    struct memchunk *chunk;
    struct sample_spec *spec;
};

static int do_mix(void *p, uint32_t index, int *del, void*userdata) {
    struct memchunk chunk;
    struct pass2_info *info = userdata;
    struct input_stream*i = p;
    assert(info && info->chunk && info->chunk->memblock && i && info->spec);
    
    if (memblockq_peek(i->memblockq, &chunk) != 0)
        return 0;

    memblock_assert_exclusive(info->chunk->memblock);
    assert(chunk.length && chunk.length <= info->chunk->memblock->length - info->chunk->index);

    add_clip(info->chunk, &chunk, info->spec);
    memblock_unref(chunk.memblock);
    memblockq_drop(i->memblockq, info->chunk->length);

    input_stream_notify(i);
    return 0;
}

int sink_render_into(struct sink*s, struct memblock *target, struct memchunk *result) {
    struct pass1_info pass1_info;
    struct pass2_info pass2_info;
    assert(s && target && result);
    memblock_assert_exclusive(target);

    /* Calculate how many bytes to mix */
    pass1_info.maxlength = target->length;
    pass1_info.count = 0;
    
    idxset_foreach(s->input_streams, get_max_length, &pass1_info);
    assert(pass1_info.maxlength);

    /* No data to mix */
    if (pass1_info.count == 0)
        return -1;
    
    /* A shortcut if only a single input stream is connected */
    if (pass1_info.count == 1) {
        struct input_stream *i = pass1_info.last_input_stream;
        struct memchunk chunk;
        size_t l;

        assert(i);
        
        if (memblockq_peek(i->memblockq, &chunk) != 0)
            return -1;

        l = target->length < chunk.length ? target->length : chunk.length;
        memcpy(target->data, result->memblock+result->index, l);
        target->length = l;
        memblock_unref(chunk.memblock);
        memblockq_drop(i->memblockq, l);

        input_stream_notify(i);
        
        result->memblock = target;
        result->length = l;
        result->index = 0;
        return 0;
    }

    /* Do the real mixing */
    result->memblock = silence(target, &s->sample_spec);
    result->index = 0;
    result->length = pass1_info.maxlength;
    pass2_info.chunk = result;
    pass2_info.spec = &s->sample_spec;
    idxset_foreach(s->input_streams, do_mix, &pass2_info);

    assert(s->monitor_source);
    source_post(s->monitor_source, result);
    
    return 0;
}

int sink_render(struct sink*s, size_t length, struct memchunk *result) {
    struct pass1_info pass1_info;
    struct pass2_info pass2_info;
    assert(s && result);

    if (!length)
        length = (size_t) -1;
    
    /* Calculate how many bytes to mix */
    pass1_info.maxlength = length;
    pass1_info.count = 0;
    
    idxset_foreach(s->input_streams, get_max_length, &pass1_info);
    assert(pass1_info.maxlength);

    /* No data to mix */
    if (pass1_info.count == 0)
        return -1;

    if (pass1_info.count == 1) {
        struct input_stream *i = pass1_info.last_input_stream;
        size_t l;

        assert(i);

        if (memblockq_peek(i->memblockq, result) != 0)
            return -1;

        l = length < result->length ? length : result->length;
        result->length = l;
        memblockq_drop(i->memblockq, l);
        input_stream_notify(i);
        
        return 0;
    }

    /* Do the mixing */
    result->memblock = silence(memblock_new(result->length), &s->sample_spec);
    result->index = 0;
    result->length = pass1_info.maxlength;
    pass2_info.chunk = result;
    pass2_info.spec = &s->sample_spec;
    idxset_foreach(s->input_streams, do_mix, &pass2_info);

    assert(s->monitor_source);

    source_post(s->monitor_source, result);
    return 0;
}

void sink_notify(struct sink*s) {
    assert(s);

    if (s->notify)
        s->notify(s, s->notify_userdata);
}

void sink_set_notify_callback(struct sink *s, void (*notify_callback)(struct sink*sink, void *userdata), void *userdata) {
    assert(s && notify_callback);

    s->notify = notify_callback;
    s->notify_userdata = userdata;
}


