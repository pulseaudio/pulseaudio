/***
    This file is part of PulseAudio.

    Copyright 2009 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <math.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>

#include "module-loopback-symdef.h"

PA_MODULE_AUTHOR("Pierre-Louis Bossart");
PA_MODULE_DESCRIPTION("Loopback from source to sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "source=<source to connect to> "
        "sink=<sink to connect to> "
        "adjust_time=<how often to readjust rates in s> "
        "latency_msec=<latency in ms> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map>");

#define DEFAULT_LATENCY_MSEC 200

#define MEMBLOCKQ_MAXLENGTH (1024*1024*16)

#define DEFAULT_ADJUST_TIME_USEC (10*PA_USEC_PER_SEC)

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_sink_input *sink_input;
    pa_source_output *source_output;

    pa_asyncmsgq *asyncmsgq;
    pa_memblockq *memblockq;

    pa_rtpoll_item *rtpoll_item_read, *rtpoll_item_write;

    pa_time_event *time_event;
    pa_usec_t adjust_time;

    int64_t recv_counter;
    int64_t send_counter;

    size_t skip;
    pa_usec_t latency;

    pa_bool_t in_pop;
    size_t min_memblockq_length;

    struct {
        int64_t send_counter;
        size_t source_output_buffer;
        pa_usec_t source_latency;

        int64_t recv_counter;
        size_t sink_input_buffer;
        pa_usec_t sink_latency;

        size_t min_memblockq_length;
        size_t max_request;
    } latency_snapshot;
};

static const char* const valid_modargs[] = {
    "source",
    "sink",
    "latency_msec",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL,
};

enum {
    SINK_INPUT_MESSAGE_POST = PA_SINK_INPUT_MESSAGE_MAX,
    SINK_INPUT_MESSAGE_REWIND,
    SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT,
    SINK_INPUT_MESSAGE_MAX_REQUEST_CHANGED
};

enum {
    SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT
};

/* Called from main context */
static void teardown(struct userdata *u) {
    pa_assert(u);
    pa_assert_ctl_context();

    if (u->sink_input)
        pa_sink_input_unlink(u->sink_input);

    if (u->source_output)
        pa_source_output_unlink(u->source_output);

    if (u->sink_input) {
        pa_sink_input_unref(u->sink_input);
        u->sink_input = NULL;
    }

    if (u->source_output) {
        pa_source_output_unref(u->source_output);
        u->source_output = NULL;
    }
}

/* Called from main context */
static void adjust_rates(struct userdata *u) {
    size_t buffer, fs;
    uint32_t old_rate, base_rate, new_rate;
    pa_usec_t buffer_latency;

    pa_assert(u);
    pa_assert_ctl_context();

    pa_asyncmsgq_send(u->source_output->source->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT, NULL, 0, NULL);
    pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT, NULL, 0, NULL);

    buffer =
        u->latency_snapshot.sink_input_buffer +
        u->latency_snapshot.source_output_buffer;

    if (u->latency_snapshot.recv_counter <= u->latency_snapshot.send_counter)
        buffer += (size_t) (u->latency_snapshot.send_counter - u->latency_snapshot.recv_counter);
    else
        buffer += PA_CLIP_SUB(buffer, (size_t) (u->latency_snapshot.recv_counter - u->latency_snapshot.send_counter));

    buffer_latency = pa_bytes_to_usec(buffer, &u->sink_input->sample_spec);

    pa_log_info("Loopback overall latency is %0.2f ms + %0.2f ms + %0.2f ms = %0.2f ms",
                (double) u->latency_snapshot.sink_latency / PA_USEC_PER_MSEC,
                (double) buffer_latency / PA_USEC_PER_MSEC,
                (double) u->latency_snapshot.source_latency / PA_USEC_PER_MSEC,
                ((double) u->latency_snapshot.sink_latency + buffer_latency + u->latency_snapshot.source_latency) / PA_USEC_PER_MSEC);

    pa_log_info("Should buffer %zu bytes, buffered at minimum %zu bytes",
                u->latency_snapshot.max_request*2,
                u->latency_snapshot.min_memblockq_length);

    fs = pa_frame_size(&u->sink_input->sample_spec);
    old_rate = u->sink_input->sample_spec.rate;
    base_rate = u->source_output->sample_spec.rate;

    if (u->latency_snapshot.min_memblockq_length < u->latency_snapshot.max_request*2)
        new_rate = base_rate - (((u->latency_snapshot.max_request*2 - u->latency_snapshot.min_memblockq_length) / fs) *PA_USEC_PER_SEC)/u->adjust_time;
    else
        new_rate = base_rate + (((u->latency_snapshot.min_memblockq_length - u->latency_snapshot.max_request*2) / fs) *PA_USEC_PER_SEC)/u->adjust_time;

    pa_log_info("Old rate %lu Hz, new rate %lu Hz", (unsigned long) old_rate, (unsigned long) new_rate);

    pa_sink_input_set_rate(u->sink_input, new_rate);

    pa_core_rttime_restart(u->core, u->time_event, pa_rtclock_now() + u->adjust_time);
}

/* Called from main context */
static void time_callback(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(a);
    pa_assert(u->time_event == e);

    adjust_rates(u);
}

/* Called from input thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct userdata *u;
    pa_memchunk copy;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    if (u->skip > chunk->length) {
        u->skip -= chunk->length;
        return;
    }

    if (u->skip > 0) {
        copy = *chunk;
        copy.index += u->skip;
        copy.length -= u->skip;
        u->skip = 0;

        chunk = &copy;
    }

    pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_POST, NULL, 0, chunk, NULL);
    u->send_counter += (int64_t) chunk->length;
}

/* Called from input thread context */
static void source_output_process_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_REWIND, NULL, (int64_t) nbytes, NULL, NULL);
    u->send_counter -= (int64_t) nbytes;
}

/* Called from output thread context */
static int source_output_process_msg_cb(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE_OUTPUT(obj)->userdata;

    switch (code) {

        case SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT: {
            size_t length;

            length = pa_memblockq_get_length(u->source_output->thread_info.delay_memblockq);

            u->latency_snapshot.send_counter = u->send_counter;
            u->latency_snapshot.source_output_buffer = u->source_output->thread_info.resampler ? pa_resampler_result(u->source_output->thread_info.resampler, length) : length;
            u->latency_snapshot.source_latency = pa_source_get_latency_within_thread(u->source_output->source);

            return 0;
        }
    }

    return pa_source_output_process_msg(obj, code, data, offset, chunk);
}

/* Called from output thread context */
static void source_output_attach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    u->rtpoll_item_write = pa_rtpoll_item_new_asyncmsgq_write(
            o->source->thread_info.rtpoll,
            PA_RTPOLL_LATE,
            u->asyncmsgq);
}

/* Called from output thread context */
static void source_output_detach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    if (u->rtpoll_item_write) {
        pa_rtpoll_item_free(u->rtpoll_item_write);
        u->rtpoll_item_write = NULL;
    }
}

/* Called from output thread context */
static void source_output_state_change_cb(pa_source_output *o, pa_source_output_state_t state) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    if (PA_SOURCE_OUTPUT_IS_LINKED(state) && o->thread_info.state == PA_SOURCE_OUTPUT_INIT) {

        u->skip = pa_usec_to_bytes(PA_CLIP_SUB(pa_source_get_latency_within_thread(o->source),
                                               u->latency),
                                   &o->sample_spec);

        pa_log_info("Skipping %lu bytes", (unsigned long) u->skip);
    }
}

/* Called from main thread */
static void source_output_kill_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    teardown(u);
    pa_module_unload_request(u->module, TRUE);
}

/* Called from main thread */
static pa_bool_t source_output_may_move_to_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    return dest != u->sink_input->sink->monitor_source;
}

/* Called from main thread */
static void source_output_moving_cb(pa_source_output *o, pa_source *dest) {
    pa_proplist *p;
    const char *n;
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    p = pa_proplist_new();
    pa_proplist_setf(p, PA_PROP_MEDIA_NAME, "Loopback of %s", pa_strnull(pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION)));

    if ((n = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_proplist_sets(p, PA_PROP_MEDIA_ICON_NAME, n);

    pa_sink_input_update_proplist(u->sink_input, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);
}

/* Called from output thread context */
static void update_min_memblockq_length(struct userdata *u) {
    size_t length;

    pa_assert(u);
    pa_sink_input_assert_io_context(u->sink_input);

    length = pa_memblockq_get_length(u->memblockq);

    if (u->min_memblockq_length == (size_t) -1 ||
        length < u->min_memblockq_length)
        u->min_memblockq_length = length;
}

/* Called from output thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);
    pa_assert(chunk);

    u->in_pop = TRUE;
    while (pa_asyncmsgq_process_one(u->asyncmsgq) > 0)
        ;
    u->in_pop = FALSE;

    if (pa_memblockq_peek(u->memblockq, chunk) < 0) {
        pa_log_info("Coud not peek into queue");
        return -1;
    }

    chunk->length = PA_MIN(chunk->length, nbytes);
    pa_memblockq_drop(u->memblockq, chunk->length);

    update_min_memblockq_length(u);

    return 0;
}

/* Called from output thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_rewind(u->memblockq, nbytes);
}

/* Called from output thread context */
static int sink_input_process_msg_cb(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK_INPUT(obj)->userdata;

    switch (code) {

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
             pa_usec_t *r = data;

             pa_sink_input_assert_io_context(u->sink_input);

            *r = pa_bytes_to_usec(pa_memblockq_get_length(u->memblockq), &u->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
        }

        case SINK_INPUT_MESSAGE_POST:

            pa_sink_input_assert_io_context(u->sink_input);

            if (PA_SINK_IS_OPENED(u->sink_input->sink->thread_info.state))
                pa_memblockq_push_align(u->memblockq, chunk);
            else
                pa_memblockq_flush_write(u->memblockq);

            update_min_memblockq_length(u);

            /* Is this the end of an underrun? Then let's start things
             * right-away */
            if (!u->in_pop &&
                u->sink_input->thread_info.underrun_for > 0 &&
                pa_memblockq_is_readable(u->memblockq)) {

                pa_log_debug("Requesting rewind due to end of underrun.");
                pa_sink_input_request_rewind(u->sink_input,
                                             (size_t) (u->sink_input->thread_info.underrun_for == (size_t) -1 ? 0 : u->sink_input->thread_info.underrun_for),
                                             FALSE, TRUE, FALSE);
            }

            u->recv_counter += (int64_t) chunk->length;

            return 0;

        case SINK_INPUT_MESSAGE_REWIND:

            pa_sink_input_assert_io_context(u->sink_input);

            if (PA_SINK_IS_OPENED(u->sink_input->sink->thread_info.state))
                pa_memblockq_seek(u->memblockq, -offset, PA_SEEK_RELATIVE, TRUE);
            else
                pa_memblockq_flush_write(u->memblockq);

            u->recv_counter -= offset;

            update_min_memblockq_length(u);

            return 0;

        case SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT: {
            size_t length;

            update_min_memblockq_length(u);

            length = pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq);

            u->latency_snapshot.recv_counter = u->recv_counter;
            u->latency_snapshot.sink_input_buffer =
                pa_memblockq_get_length(u->memblockq) +
                (u->sink_input->thread_info.resampler ? pa_resampler_request(u->sink_input->thread_info.resampler, length) : length);
            u->latency_snapshot.sink_latency = pa_sink_get_latency_within_thread(u->sink_input->sink);

            u->latency_snapshot.max_request = pa_sink_input_get_max_request(u->sink_input);

            u->latency_snapshot.min_memblockq_length = u->min_memblockq_length;
            u->min_memblockq_length = (size_t) -1;

            return 0;
        }

        case SINK_INPUT_MESSAGE_MAX_REQUEST_CHANGED: {
            /* This message is sent from the IO thread to the main
             * thread! So don't be confused. All the user cases above
             * are executed in thread context, but this one is not! */

            pa_assert_ctl_context();

            adjust_rates(u);
            return 0;
        }
    }

    return pa_sink_input_process_msg(obj, code, data, offset, chunk);
}

/* Called from output thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    u->rtpoll_item_read = pa_rtpoll_item_new_asyncmsgq_read(
            i->sink->thread_info.rtpoll,
            PA_RTPOLL_LATE,
            u->asyncmsgq);

    pa_memblockq_set_prebuf(u->memblockq, pa_sink_input_get_max_request(i)*2);
    pa_memblockq_set_maxrewind(u->memblockq, pa_sink_input_get_max_rewind(i));

    u->min_memblockq_length = (size_t) -1;
}

/* Called from output thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    if (u->rtpoll_item_read) {
        pa_rtpoll_item_free(u->rtpoll_item_read);
        u->rtpoll_item_read = NULL;
    }
}

/* Called from output thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_set_maxrewind(u->memblockq, nbytes);
}

/* Called from output thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_set_prebuf(u->memblockq, nbytes*2);
    pa_log_info("Max request changed");
    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_MAX_REQUEST_CHANGED, NULL, 0, NULL, NULL);
}

/* Called from main thread */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    teardown(u);
    pa_module_unload_request(u->module, TRUE);
}

/* Called from main thread */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;
    pa_proplist *p;
    const char *n;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    p = pa_proplist_new();
    pa_proplist_setf(p, PA_PROP_MEDIA_NAME, "Loopback to %s", pa_strnull(pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION)));

    if ((n = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_proplist_sets(p, PA_PROP_MEDIA_ICON_NAME, n);

    pa_source_output_update_proplist(u->source_output, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);
}

/* Called from main thread */
static pa_bool_t sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    if (!u->source_output->source->monitor_of)
        return TRUE;

    return dest != u->source_output->source->monitor_of;
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    pa_sink *sink;
    pa_sink_input_new_data sink_input_data;
    pa_source *source;
    pa_source_output_new_data source_output_data;
    uint32_t latency_msec;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_memchunk silence;
    uint32_t adjust_time_sec;
    const char *n;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!(source = pa_namereg_get(m->core, pa_modargs_get_value(ma, "source", NULL), PA_NAMEREG_SOURCE))) {
        pa_log("No such source.");
        goto fail;
    }

    if (!(sink = pa_namereg_get(m->core, pa_modargs_get_value(ma, "sink", NULL), PA_NAMEREG_SINK))) {
        pa_log("No such sink.");
        goto fail;
    }

    ss = sink->sample_spec;
    map = sink->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    latency_msec = DEFAULT_LATENCY_MSEC;
    if (pa_modargs_get_value_u32(ma, "latency_msec", &latency_msec) < 0 || latency_msec < 1 || latency_msec > 2000) {
        pa_log("Invalid latency specification");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->latency = (pa_usec_t) latency_msec * PA_USEC_PER_MSEC;

    adjust_time_sec = DEFAULT_ADJUST_TIME_USEC / PA_USEC_PER_SEC;
    if (pa_modargs_get_value_u32(ma, "adjust_time", &adjust_time_sec) < 0) {
        pa_log("Failed to parse adjust_time value");
        goto fail;
    }

    if (adjust_time_sec != DEFAULT_ADJUST_TIME_USEC / PA_USEC_PER_SEC)
        u->adjust_time = adjust_time_sec * PA_USEC_PER_SEC;
    else
        u->adjust_time = DEFAULT_ADJUST_TIME_USEC;

    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    sink_input_data.sink = sink;

    pa_proplist_setf(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Loopback of %s",
                     pa_strnull(pa_proplist_gets(source->proplist, PA_PROP_DEVICE_DESCRIPTION)));
    if ((n = pa_proplist_gets(source->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ICON_NAME, n);
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "abstract");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);
    sink_input_data.flags = PA_SINK_INPUT_VARIABLE_RATE;

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    u->sink_input->parent.process_msg = sink_input_process_msg_cb;
    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->userdata = u;

    pa_sink_input_set_requested_latency(u->sink_input, u->latency/3);

    pa_source_output_new_data_init(&source_output_data);
    source_output_data.driver = __FILE__;
    source_output_data.module = m;
    source_output_data.source = source;
    pa_proplist_setf(source_output_data.proplist, PA_PROP_MEDIA_NAME, "Loopback to %s",
                     pa_strnull(pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));
    if ((n = pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_ICON_NAME, n);
    pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_ROLE, "abstract");
    pa_source_output_new_data_set_sample_spec(&source_output_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);

    pa_source_output_new(&u->source_output, m->core, &source_output_data);
    pa_source_output_new_data_done(&source_output_data);

    if (!u->source_output)
        goto fail;

    u->source_output->parent.process_msg = source_output_process_msg_cb;
    u->source_output->push = source_output_push_cb;
    u->source_output->process_rewind = source_output_process_rewind_cb;
    u->source_output->kill = source_output_kill_cb;
    u->source_output->attach = source_output_attach_cb;
    u->source_output->detach = source_output_detach_cb;
    u->source_output->state_change = source_output_state_change_cb;
    u->source_output->may_move_to = source_output_may_move_to_cb;
    u->source_output->moving = source_output_moving_cb;
    u->source_output->userdata = u;

    pa_source_output_set_requested_latency(u->source_output, u->latency/3);

    pa_sink_input_get_silence(u->sink_input, &silence);
    u->memblockq = pa_memblockq_new(
            0,                      /* idx */
            MEMBLOCKQ_MAXLENGTH,    /* maxlength */
            MEMBLOCKQ_MAXLENGTH,    /* tlength */
            pa_frame_size(&ss),     /* base */
            0,                      /* prebuf */
            0,                      /* minreq */
            0,                      /* maxrewind */
            &silence);              /* silence frame */
    pa_memblock_unref(silence.memblock);

    u->asyncmsgq = pa_asyncmsgq_new(0);

    pa_sink_input_put(u->sink_input);
    pa_source_output_put(u->source_output);

    if (u->adjust_time > 0)
        u->time_event = pa_core_rttime_new(m->core, pa_rtclock_now() + u->adjust_time, time_callback, u);

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    teardown(u);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    if (u->asyncmsgq)
        pa_asyncmsgq_unref(u->asyncmsgq);

    if (u->time_event)
        u->core->mainloop->time_free(u->time_event);

    pa_xfree(u);
}
