#ifndef foopolyphfoo
#define foopolyphfoo

#include <sys/types.h>

#include "sample.h"
#include "polypdef.h"
#include "mainloop-api.h"

struct pa_context;

struct pa_context *pa_context_new(struct pa_mainloop_api *mainloop, const char *name);

int pa_context_connect(
    struct pa_context *c,
    const char *server,
    void (*complete) (struct pa_context*c, int success, void *userdata),
    void *userdata);

int pa_context_drain(
    struct pa_context *c, 
    void (*complete) (struct pa_context*c, void *userdata),
    void *userdata);

void pa_context_free(struct pa_context *c);

void pa_context_set_die_callback(struct pa_context *c, void (*cb)(struct pa_context *c, void *userdata), void *userdata);

int pa_context_is_dead(struct pa_context *c);
int pa_context_is_ready(struct pa_context *c);
int pa_context_errno(struct pa_context *c);

int pa_context_is_pending(struct pa_context *c);

struct pa_stream;

struct pa_stream* pa_stream_new(
    struct pa_context *c,
    enum pa_stream_direction dir,
    const char *dev,
    const char *name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr,
    void (*complete) (struct pa_stream*s, int success, void *userdata),
    void *userdata);

void pa_stream_free(struct pa_stream *p);

void pa_stream_set_die_callback(struct pa_stream *s, void (*cb)(struct pa_stream *s, void *userdata), void *userdata);

void pa_stream_set_write_callback(struct pa_stream *p, void (*cb)(struct pa_stream *p, size_t length, void *userdata), void *userdata);
void pa_stream_write(struct pa_stream *p, const void *data, size_t length);
size_t pa_stream_writable_size(struct pa_stream *p);

void pa_stream_set_read_callback(struct pa_stream *p, void (*cb)(struct pa_stream *p, const void*data, size_t length, void *userdata), void *userdata);

int pa_stream_is_dead(struct pa_stream *p);
int pa_stream_is_ready(struct pa_stream*p);

struct pa_context* pa_stream_get_context(struct pa_stream *p);

#endif
