#ifndef foopolyplibhfoo
#define foopolyplibhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>

#include "sample.h"
#include "polyplib-def.h"
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

void pa_context_exit(struct pa_context *c);
void pa_context_stat(struct pa_context *c, void (*cb)(struct pa_context *c, uint32_t count, uint32_t total, void *userdata), void *userdata);

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

void pa_stream_drain(
    struct pa_stream *s, 
    void (*complete) (struct pa_stream*s, void *userdata),
    void *userdata);


void pa_stream_set_die_callback(struct pa_stream *s, void (*cb)(struct pa_stream *s, void *userdata), void *userdata);

void pa_stream_set_write_callback(struct pa_stream *p, void (*cb)(struct pa_stream *p, size_t length, void *userdata), void *userdata);
void pa_stream_write(struct pa_stream *p, const void *data, size_t length);
size_t pa_stream_writable_size(struct pa_stream *p);

void pa_stream_set_read_callback(struct pa_stream *p, void (*cb)(struct pa_stream *p, const void*data, size_t length, void *userdata), void *userdata);

int pa_stream_is_dead(struct pa_stream *p);
int pa_stream_is_ready(struct pa_stream*p);

void pa_stream_get_latency(struct pa_stream *p, void (*cb)(struct pa_stream *p, uint32_t latency, void *userdata), void *userdata);

struct pa_context* pa_stream_get_context(struct pa_stream *p);

struct pa_stream* pa_context_upload_sample(struct pa_context *c, const char *name, const struct pa_sample_spec *ss, size_t length, void (*cb) (struct pa_stream*s, int success, void *userdata), void *userdata);
void pa_stream_finish_sample(struct pa_stream *p, void (*cb)(struct pa_stream*s, int success, void *userdata), void *userdata);

void pa_context_play_sample(struct pa_context *c, const char *name, const char *dev, uint32_t volume, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);
void pa_context_remove_sample(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);

struct pa_sink_info {
    const char *name;
    uint32_t index;
    const char *description;
    struct pa_sample_spec *sample_spec;
    uint32_t owner_module;
    uint32_t volume;
    uint32_t monitor_source;
    const char *monitor_source_name;
    uint32_t latency;
};

void pa_context_get_sink_info_by_name(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, void *userdata), void *userdata);
void pa_context_get_sink_info_by_id(struct pa_context *c, uint32_t id, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, void *userdata), void *userdata);
void pa_context_get_sink_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, void *userdata), void *userdata);

struct pa_source_info {
    const char *name;
    uint32_t index;
    const char *description;
    struct pa_sample_spec *sample_spec;
    uint32_t owner_module;
    uint32_t monitor_of_sink;
    const char *monitor_of_sink_name;
};

void pa_context_get_source_info_by_name(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, const struct pa_source_info *i, void *userdata), void *userdata);
void pa_context_get_source_info_by_id(struct pa_context *c, uint32_t id, void (*cb)(struct pa_context *c, const struct pa_source_info *i, void *userdata), void *userdata);
void pa_context_get_source_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_source_info *i, void *userdata), void *userdata);

#endif
