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
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

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
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "source=<source to connect to> "
        "sink=<sink to connect to> "
        "adjust_time=<how often to readjust rates in s> "
        "latency_msec=<latency in ms> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        "sink_input_properties=<proplist> "
        "source_output_properties=<proplist> "
        "source_dont_move=<boolean> "
        "sink_dont_move=<boolean> "
        "remix=<remix channels?> ");

#define DEFAULT_LATENCY_MSEC 200

#define MEMBLOCKQ_MAXLENGTH (1024*1024*32)

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

    bool in_pop;

    struct {
        int64_t send_counter;
        pa_usec_t source_latency;
        pa_usec_t source_timestamp;

        int64_t recv_counter;
        size_t sink_input_buffer;
        pa_usec_t sink_latency;
        pa_usec_t sink_timestamp;
    } latency_snapshot;
};

static const char* const valid_modargs[] = {
    "source",
    "sink",
    "adjust_time",
    "latency_msec",
    "format",
    "rate",
    "channels",
    "channel_map",
    "sink_input_properties",
    "source_output_properties",
    "source_dont_move",
    "sink_dont_move",
    "remix",
    NULL,
};

enum {
    SINK_INPUT_MESSAGE_POST = PA_SINK_INPUT_MESSAGE_MAX,
    SINK_INPUT_MESSAGE_REWIND,
    SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT
};

enum {
    SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT = PA_SOURCE_OUTPUT_MESSAGE_MAX,
};

static void enable_adjust_timer(struct userdata *u, bool enable);

/* Called from main context */
static void teardown(struct userdata *u) {
    pa_assert(u);
    pa_assert_ctl_context();

    u->adjust_time = 0;
    enable_adjust_timer(u, false);

    /* Handling the asyncmsgq between the source output and the sink input
     * requires some care. When the source output is unlinked, nothing needs
     * to be done for the asyncmsgq, because the source output is the sending
     * end. But when the sink input is unlinked, we should ensure that the
     * asyncmsgq is emptied, because the messages in the queue hold references
     * to the sink input. Also, we need to ensure that new messages won't be
     * written to the queue after we have emptied it.
     *
     * Emptying the queue can be done in the state_changed() callback of the
     * sink input, when the new state is "unlinked".
     *
     * Preventing new messages from being written to the queue can be achieved
     * by unlinking the source output before unlinking the sink input. There
     * are no other writers for that queue, so this is sufficient. */

    if (u->source_output) {
        pa_source_output_unlink(u->source_output);
        pa_source_output_unref(u->source_output);
        u->source_output = NULL;
    }

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
        u->sink_input = NULL;
    }
}

/* rate controller
 * - maximum deviation from base rate is less than 1%
 * - can create audible artifacts by changing the rate too quickly
 * - exhibits hunting with USB or Bluetooth sources
 */
static uint32_t rate_controller(
                uint32_t base_rate,
                pa_usec_t adjust_time,
                int32_t latency_difference_usec) {

    uint32_t new_rate;
    double min_cycles;

    /* Calculate best rate to correct the current latency offset, limit at
     * slightly below 1% difference from base_rate */
    min_cycles = (double)abs(latency_difference_usec) / adjust_time / 0.01 + 1;
    new_rate = base_rate * (1.0 + (double)latency_difference_usec / min_cycles / adjust_time);

    return new_rate;
}

/* Called from main context */
static void adjust_rates(struct userdata *u) {
    size_t buffer;
    uint32_t old_rate, base_rate, new_rate;
    int32_t latency_difference;
    pa_usec_t current_buffer_latency, snapshot_delay, current_source_sink_latency, current_latency, latency_at_optimum_rate;
    pa_usec_t final_latency;

    pa_assert(u);
    pa_assert_ctl_context();

    /* Rates and latencies*/
    old_rate = u->sink_input->sample_spec.rate;
    base_rate = u->source_output->sample_spec.rate;

    buffer = u->latency_snapshot.sink_input_buffer;
    if (u->latency_snapshot.recv_counter <= u->latency_snapshot.send_counter)
        buffer += (size_t) (u->latency_snapshot.send_counter - u->latency_snapshot.recv_counter);
    else
        buffer = PA_CLIP_SUB(buffer, (size_t) (u->latency_snapshot.recv_counter - u->latency_snapshot.send_counter));

    current_buffer_latency = pa_bytes_to_usec(buffer, &u->sink_input->sample_spec);
    snapshot_delay = u->latency_snapshot.source_timestamp - u->latency_snapshot.sink_timestamp;
    current_source_sink_latency = u->latency_snapshot.sink_latency + u->latency_snapshot.source_latency - snapshot_delay;

    /* Current latency */
    current_latency = current_source_sink_latency + current_buffer_latency;

    /* Latency at base rate */
    latency_at_optimum_rate = current_source_sink_latency + current_buffer_latency * old_rate / base_rate;

    final_latency = u->latency;
    latency_difference = (int32_t)((int64_t)latency_at_optimum_rate - final_latency);

    pa_log_debug("Loopback overall latency is %0.2f ms + %0.2f ms + %0.2f ms = %0.2f ms",
                (double) u->latency_snapshot.sink_latency / PA_USEC_PER_MSEC,
                (double) current_buffer_latency / PA_USEC_PER_MSEC,
                (double) u->latency_snapshot.source_latency / PA_USEC_PER_MSEC,
                (double) current_latency / PA_USEC_PER_MSEC);

    pa_log_debug("Loopback latency at base rate is %0.2f ms", (double)latency_at_optimum_rate / PA_USEC_PER_MSEC);

    /* Calculate new rate */
    new_rate = rate_controller(base_rate, u->adjust_time, latency_difference);

    /* Set rate */
    pa_sink_input_set_rate(u->sink_input, new_rate);
    pa_log_debug("[%s] Updated sampling rate to %lu Hz.", u->sink_input->sink->name, (unsigned long) new_rate);
}

/* Called from main context */
static void time_callback(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(a);
    pa_assert(u->time_event == e);

    /* Restart timer right away */
    pa_core_rttime_restart(u->core, u->time_event, pa_rtclock_now() + u->adjust_time);

    /* Get sink and source latency snapshot */
    pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT, NULL, 0, NULL);
    pa_asyncmsgq_send(u->source_output->source->asyncmsgq, PA_MSGOBJECT(u->source_output), SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT, NULL, 0, NULL);

    adjust_rates(u);
}

/* Called from main context
 * When source or sink changes, give it a third of a second to settle down, then call adjust_rates for the first time */
static void enable_adjust_timer(struct userdata *u, bool enable) {
    if (enable) {
        if (!u->adjust_time)
            return;
        if (u->time_event)
            u->core->mainloop->time_free(u->time_event);

        u->time_event = pa_core_rttime_new(u->module->core, pa_rtclock_now() + 333 * PA_USEC_PER_MSEC, time_callback, u);
    } else {
        if (!u->time_event)
            return;

        u->core->mainloop->time_free(u->time_event);
        u->time_event = NULL;
    }
}

/* Called from main context */
static void update_adjust_timer(struct userdata *u) {
    if (u->sink_input->state == PA_SINK_INPUT_CORKED || u->source_output->state == PA_SOURCE_OUTPUT_CORKED)
        enable_adjust_timer(u, false);
    else
        enable_adjust_timer(u, true);
}

/* Called from input thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct userdata *u;
    pa_memchunk copy;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    if (u->skip >= chunk->length) {
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
            /* Add content of delay memblockq to the source latency */
            u->latency_snapshot.source_latency = pa_source_get_latency_within_thread(u->source_output->source) +
                                                 pa_bytes_to_usec(length, &u->source_output->source->sample_spec);
            u->latency_snapshot.source_timestamp = pa_rtclock_now();

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
    pa_module_unload_request(u->module, true);
}

/* Called from main thread */
static bool source_output_may_move_to_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    if (!u->sink_input || !u->sink_input->sink)
        return true;

    return dest != u->sink_input->sink->monitor_source;
}

/* Called from main thread */
static void source_output_moving_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;
    char *input_description;
    const char *n;

    if (!dest)
        return;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    input_description = pa_sprintf_malloc("Loopback of %s",
                                          pa_strnull(pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION)));
    pa_sink_input_set_property(u->sink_input, PA_PROP_MEDIA_NAME, input_description);
    pa_xfree(input_description);

    if ((n = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_sink_input_set_property(u->sink_input, PA_PROP_DEVICE_ICON_NAME, n);

    if (pa_source_get_state(dest) == PA_SOURCE_SUSPENDED)
        pa_sink_input_cork(u->sink_input, true);
    else
        pa_sink_input_cork(u->sink_input, false);

    update_adjust_timer(u);
}

/* Called from main thread */
static void source_output_suspend_cb(pa_source_output *o, bool suspended) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    pa_sink_input_cork(u->sink_input, suspended);

    update_adjust_timer(u);
}

/* Called from output thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);
    pa_assert(chunk);

    u->in_pop = true;
    while (pa_asyncmsgq_process_one(u->asyncmsgq) > 0)
        ;
    u->in_pop = false;

    if (pa_memblockq_peek(u->memblockq, chunk) < 0) {
        pa_log_info("Could not peek into queue");
        return -1;
    }

    chunk->length = PA_MIN(chunk->length, nbytes);
    pa_memblockq_drop(u->memblockq, chunk->length);

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
                pa_memblockq_flush_write(u->memblockq, true);

            /* Is this the end of an underrun? Then let's start things
             * right-away */
            if (!u->in_pop &&
                u->sink_input->thread_info.underrun_for > 0 &&
                pa_memblockq_is_readable(u->memblockq)) {

                pa_log_debug("Requesting rewind due to end of underrun.");
                pa_sink_input_request_rewind(u->sink_input,
                                             (size_t) (u->sink_input->thread_info.underrun_for == (size_t) -1 ? 0 : u->sink_input->thread_info.underrun_for),
                                             false, true, false);
            }

            u->recv_counter += (int64_t) chunk->length;

            return 0;

        case SINK_INPUT_MESSAGE_REWIND:

            pa_sink_input_assert_io_context(u->sink_input);

            if (PA_SINK_IS_OPENED(u->sink_input->sink->thread_info.state))
                pa_memblockq_seek(u->memblockq, -offset, PA_SEEK_RELATIVE, true);
            else
                pa_memblockq_flush_write(u->memblockq, true);

            u->recv_counter -= offset;

            return 0;

        case SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT: {
            size_t length;

            length = pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq);

            u->latency_snapshot.recv_counter = u->recv_counter;
            u->latency_snapshot.sink_input_buffer = pa_memblockq_get_length(u->memblockq);
            /* Add content of render memblockq to sink latency */
            u->latency_snapshot.sink_latency = pa_sink_get_latency_within_thread(u->sink_input->sink) +
                                               pa_bytes_to_usec(length, &u->sink_input->sink->sample_spec);
            u->latency_snapshot.sink_timestamp = pa_rtclock_now();

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
}

/* Called from main thread */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    teardown(u);
    pa_module_unload_request(u->module, true);
}

/* Called from the output thread context */
static void sink_input_state_change_cb(pa_sink_input *i, pa_sink_input_state_t state) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (state == PA_SINK_INPUT_UNLINKED)
        pa_asyncmsgq_flush(u->asyncmsgq, false);
}

/* Called from main thread */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;
    char *output_description;
    const char *n;

    if (!dest)
        return;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    output_description = pa_sprintf_malloc("Loopback to %s",
                                           pa_strnull(pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION)));
    pa_source_output_set_property(u->source_output, PA_PROP_MEDIA_NAME, output_description);
    pa_xfree(output_description);

    if ((n = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_source_output_set_property(u->source_output, PA_PROP_MEDIA_ICON_NAME, n);

    if (pa_sink_get_state(dest) == PA_SINK_SUSPENDED)
        pa_source_output_cork(u->source_output, true);
    else
        pa_source_output_cork(u->source_output, false);

    update_adjust_timer(u);
}

/* Called from main thread */
static bool sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    if (!u->source_output || !u->source_output->source)
        return true;

    return dest != u->source_output->source->monitor_of;
}

/* Called from main thread */
static void sink_input_suspend_cb(pa_sink_input *i, bool suspended) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    pa_source_output_cork(u->source_output, suspended);

    update_adjust_timer(u);
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    pa_sink *sink = NULL;
    pa_sink_input_new_data sink_input_data;
    bool sink_dont_move;
    pa_source *source = NULL;
    pa_source_output_new_data source_output_data;
    bool source_dont_move;
    uint32_t latency_msec;
    pa_sample_spec ss;
    pa_channel_map map;
    bool format_set = false;
    bool rate_set = false;
    bool channels_set = false;
    pa_memchunk silence;
    uint32_t adjust_time_sec;
    const char *n;
    bool remix = true;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    n = pa_modargs_get_value(ma, "source", NULL);
    if (n && !(source = pa_namereg_get(m->core, n, PA_NAMEREG_SOURCE))) {
        pa_log("No such source.");
        goto fail;
    }

    n = pa_modargs_get_value(ma, "sink", NULL);
    if (n && !(sink = pa_namereg_get(m->core, n, PA_NAMEREG_SINK))) {
        pa_log("No such sink.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "remix", &remix) < 0) {
        pa_log("Invalid boolean remix parameter");
        goto fail;
    }

    if (sink) {
        ss = sink->sample_spec;
        map = sink->channel_map;
        format_set = true;
        rate_set = true;
        channels_set = true;
    } else if (source) {
        ss = source->sample_spec;
        map = source->channel_map;
        format_set = true;
        rate_set = true;
        channels_set = true;
    } else {
        /* FIXME: Dummy stream format, needed because pa_sink_input_new()
         * requires valid sample spec and channel map even when all the FIX_*
         * stream flags are specified. pa_sink_input_new() should be changed
         * to ignore the sample spec and channel map when the FIX_* flags are
         * present. */
        ss.format = PA_SAMPLE_U8;
        ss.rate = 8000;
        ss.channels = 1;
        map.channels = 1;
        map.map[0] = PA_CHANNEL_POSITION_MONO;
    }

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    if (ss.rate < 4000 || ss.rate > PA_RATE_MAX) {
        pa_log("Invalid rate specification, valid range is 4000 Hz to %i Hz", PA_RATE_MAX);
        goto fail;
    }

    if (pa_modargs_get_value(ma, "format", NULL))
        format_set = true;

    if (pa_modargs_get_value(ma, "rate", NULL))
        rate_set = true;

    if (pa_modargs_get_value(ma, "channels", NULL) || pa_modargs_get_value(ma, "channel_map", NULL))
        channels_set = true;

    latency_msec = DEFAULT_LATENCY_MSEC;
    if (pa_modargs_get_value_u32(ma, "latency_msec", &latency_msec) < 0 || latency_msec < 1 || latency_msec > 30000) {
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

    if (sink)
        pa_sink_input_new_data_set_sink(&sink_input_data, sink, false);

    if (pa_modargs_get_proplist(ma, "sink_input_properties", sink_input_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Failed to parse the sink_input_properties value.");
        pa_sink_input_new_data_done(&sink_input_data);
        goto fail;
    }

    if (!pa_proplist_contains(sink_input_data.proplist, PA_PROP_MEDIA_ROLE))
        pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "abstract");

    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);
    sink_input_data.flags = PA_SINK_INPUT_VARIABLE_RATE | PA_SINK_INPUT_START_CORKED;

    if (!remix)
        sink_input_data.flags |= PA_SINK_INPUT_NO_REMIX;

    if (!format_set)
        sink_input_data.flags |= PA_SINK_INPUT_FIX_FORMAT;

    if (!rate_set)
        sink_input_data.flags |= PA_SINK_INPUT_FIX_RATE;

    if (!channels_set)
        sink_input_data.flags |= PA_SINK_INPUT_FIX_CHANNELS;

    sink_dont_move = false;
    if (pa_modargs_get_value_boolean(ma, "sink_dont_move", &sink_dont_move) < 0) {
        pa_log("sink_dont_move= expects a boolean argument.");
        goto fail;
    }

    if (sink_dont_move)
        sink_input_data.flags |= PA_SINK_INPUT_DONT_MOVE;

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    /* If format, rate or channels were originally unset, they are set now
     * after the pa_sink_input_new() call. */
    ss = u->sink_input->sample_spec;
    map = u->sink_input->channel_map;

    u->sink_input->parent.process_msg = sink_input_process_msg_cb;
    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->state_change = sink_input_state_change_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->suspend = sink_input_suspend_cb;
    u->sink_input->userdata = u;

    pa_sink_input_set_requested_latency(u->sink_input, u->latency/3);

    pa_source_output_new_data_init(&source_output_data);
    source_output_data.driver = __FILE__;
    source_output_data.module = m;
    if (source)
        pa_source_output_new_data_set_source(&source_output_data, source, false);

    if (pa_modargs_get_proplist(ma, "source_output_properties", source_output_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Failed to parse the source_output_properties value.");
        pa_source_output_new_data_done(&source_output_data);
        goto fail;
    }

    if (!pa_proplist_contains(source_output_data.proplist, PA_PROP_MEDIA_ROLE))
        pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_ROLE, "abstract");

    pa_source_output_new_data_set_sample_spec(&source_output_data, &ss);
    pa_source_output_new_data_set_channel_map(&source_output_data, &map);
    source_output_data.flags = PA_SOURCE_OUTPUT_START_CORKED;

    if (!remix)
        source_output_data.flags |= PA_SOURCE_OUTPUT_NO_REMIX;

    source_dont_move = false;
    if (pa_modargs_get_value_boolean(ma, "source_dont_move", &source_dont_move) < 0) {
        pa_log("source_dont_move= expects a boolean argument.");
        goto fail;
    }

    if (source_dont_move)
        source_output_data.flags |= PA_SOURCE_OUTPUT_DONT_MOVE;

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
    u->source_output->suspend = source_output_suspend_cb;
    u->source_output->userdata = u;

    pa_source_output_set_requested_latency(u->source_output, u->latency/3);

    pa_sink_input_get_silence(u->sink_input, &silence);
    u->memblockq = pa_memblockq_new(
            "module-loopback memblockq",
            0,                      /* idx */
            MEMBLOCKQ_MAXLENGTH,    /* maxlength */
            MEMBLOCKQ_MAXLENGTH,    /* tlength */
            &ss,                    /* sample_spec */
            0,                      /* prebuf */
            0,                      /* minreq */
            0,                      /* maxrewind */
            &silence);              /* silence frame */
    pa_memblock_unref(silence.memblock);

    u->asyncmsgq = pa_asyncmsgq_new(0);
    if (!u->asyncmsgq) {
        pa_log("pa_asyncmsgq_new() failed.");
        goto fail;
    }

    if (!pa_proplist_contains(u->source_output->proplist, PA_PROP_MEDIA_NAME))
        pa_proplist_setf(u->source_output->proplist, PA_PROP_MEDIA_NAME, "Loopback to %s",
                         pa_strnull(pa_proplist_gets(u->sink_input->sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));

    if (!pa_proplist_contains(u->source_output->proplist, PA_PROP_MEDIA_ICON_NAME)
            && (n = pa_proplist_gets(u->sink_input->sink->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_proplist_sets(u->source_output->proplist, PA_PROP_MEDIA_ICON_NAME, n);

    if (!pa_proplist_contains(u->sink_input->proplist, PA_PROP_MEDIA_NAME))
        pa_proplist_setf(u->sink_input->proplist, PA_PROP_MEDIA_NAME, "Loopback from %s",
                         pa_strnull(pa_proplist_gets(u->source_output->source->proplist, PA_PROP_DEVICE_DESCRIPTION)));

    if (source && !pa_proplist_contains(u->sink_input->proplist, PA_PROP_MEDIA_ICON_NAME)
            && (n = pa_proplist_gets(u->source_output->source->proplist, PA_PROP_DEVICE_ICON_NAME)))
        pa_proplist_sets(u->sink_input->proplist, PA_PROP_MEDIA_ICON_NAME, n);

    pa_sink_input_put(u->sink_input);
    pa_source_output_put(u->source_output);

    if (pa_source_get_state(u->source_output->source) != PA_SOURCE_SUSPENDED)
        pa_sink_input_cork(u->sink_input, false);

    if (pa_sink_get_state(u->sink_input->sink) != PA_SINK_SUSPENDED)
        pa_source_output_cork(u->source_output, false);

    update_adjust_timer(u);

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

    pa_xfree(u);
}
