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

PA_MODULE_AUTHOR("Pierre-Louis Bossart");
PA_MODULE_DESCRIPTION("Loopback from source to sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "source=<source to connect to> "
        "sink=<sink to connect to> "
        "adjust_time=<how often to readjust rates in s> "
        "latency_msec=<latency in ms> "
        "max_latency_msec=<maximum latency in ms> "
        "fast_adjust_threshold_msec=<threshold for fast adjust in ms> "
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

#define MIN_DEVICE_LATENCY (2.5*PA_USEC_PER_MSEC)

#define DEFAULT_ADJUST_TIME_USEC (10*PA_USEC_PER_SEC)

typedef struct loopback_msg loopback_msg;

struct userdata {
    pa_core *core;
    pa_module *module;

    loopback_msg *msg;

    pa_sink_input *sink_input;
    pa_source_output *source_output;

    pa_asyncmsgq *asyncmsgq;
    pa_memblockq *memblockq;

    pa_rtpoll_item *rtpoll_item_read, *rtpoll_item_write;

    pa_time_event *time_event;

    /* Variables used to calculate the average time between
     * subsequent calls of adjust_rates() */
    pa_usec_t adjust_time_stamp;
    pa_usec_t real_adjust_time;
    pa_usec_t real_adjust_time_sum;

    /* Values from command line configuration */
    pa_usec_t latency;
    pa_usec_t max_latency;
    pa_usec_t adjust_time;
    pa_usec_t fast_adjust_threshold;

    /* Latency boundaries and current values */
    pa_usec_t min_source_latency;
    pa_usec_t max_source_latency;
    pa_usec_t min_sink_latency;
    pa_usec_t max_sink_latency;
    pa_usec_t configured_sink_latency;
    pa_usec_t configured_source_latency;
    int64_t source_latency_offset;
    int64_t sink_latency_offset;
    pa_usec_t minimum_latency;

    /* lower latency limit found by underruns */
    pa_usec_t underrun_latency_limit;

    /* Various counters */
    uint32_t iteration_counter;
    uint32_t underrun_counter;
    uint32_t adjust_counter;

    bool fixed_alsa_source;
    bool source_sink_changed;

    /* Used for sink input and source output snapshots */
    struct {
        int64_t send_counter;
        int64_t source_latency;
        pa_usec_t source_timestamp;

        int64_t recv_counter;
        size_t loopback_memblockq_length;
        int64_t sink_latency;
        pa_usec_t sink_timestamp;
    } latency_snapshot;

    /* Input thread variable */
    int64_t send_counter;

    /* Output thread variables */
    struct {
        int64_t recv_counter;
        pa_usec_t effective_source_latency;

        /* Copied from main thread */
        pa_usec_t minimum_latency;

        /* Various booleans */
        bool in_pop;
        bool pop_called;
        bool pop_adjust;
        bool first_pop_done;
        bool push_called;
    } output_thread_info;
};

struct loopback_msg {
    pa_msgobject parent;
    struct userdata *userdata;
};

PA_DEFINE_PRIVATE_CLASS(loopback_msg, pa_msgobject);
#define LOOPBACK_MSG(o) (loopback_msg_cast(o))

static const char* const valid_modargs[] = {
    "source",
    "sink",
    "adjust_time",
    "latency_msec",
    "max_latency_msec",
    "fast_adjust_threshold_msec",
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
    SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT,
    SINK_INPUT_MESSAGE_SOURCE_CHANGED,
    SINK_INPUT_MESSAGE_SET_EFFECTIVE_SOURCE_LATENCY,
    SINK_INPUT_MESSAGE_UPDATE_MIN_LATENCY,
    SINK_INPUT_MESSAGE_FAST_ADJUST,
};

enum {
    SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT = PA_SOURCE_OUTPUT_MESSAGE_MAX,
};

enum {
    LOOPBACK_MESSAGE_SOURCE_LATENCY_RANGE_CHANGED,
    LOOPBACK_MESSAGE_SINK_LATENCY_RANGE_CHANGED,
    LOOPBACK_MESSAGE_UNDERRUN,
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
     * Emptying the queue can be done in the state_change() callback of the
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

/* rate controller, called from main context
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

/* Called from main thread.
 * It has been a matter of discussion how to correctly calculate the minimum
 * latency that module-loopback can deliver with a given source and sink.
 * The calculation has been placed in a separate function so that the definition
 * can easily be changed. The resulting estimate is not very exact because it
 * depends on the reported latency ranges. In cases were the lower bounds of
 * source and sink latency are not reported correctly (USB) the result will
 * be wrong. */
static void update_minimum_latency(struct userdata *u, pa_sink *sink, bool print_msg) {

    if (u->underrun_latency_limit)
        /* If we already detected a real latency limit because of underruns, use it */
        u->minimum_latency = u->underrun_latency_limit;

    else {
        /* Calculate latency limit from latency ranges */

        u->minimum_latency = u->min_sink_latency;
        if (u->fixed_alsa_source)
            /* If we are using an alsa source with fixed latency, we will get a wakeup when
             * one fragment is filled, and then we empty the source buffer, so the source
             * latency never grows much beyond one fragment (assuming that the CPU doesn't
             * cause a bottleneck). */
            u->minimum_latency += u->core->default_fragment_size_msec * PA_USEC_PER_MSEC;

        else
            /* In all other cases the source will deliver new data at latest after one source latency.
             * Make sure there is enough data available that the sink can keep on playing until new
             * data is pushed. */
            u->minimum_latency += u->min_source_latency;

        /* Multiply by 1.1 as a safety margin for delays that are proportional to the buffer sizes */
        u->minimum_latency *= 1.1;

        /* Add 1.5 ms as a safety margin for delays not related to the buffer sizes */
        u->minimum_latency += 1.5 * PA_USEC_PER_MSEC;
    }

    /* Add the latency offsets */
    if (-(u->sink_latency_offset + u->source_latency_offset) <= (int64_t)u->minimum_latency)
        u->minimum_latency += u->sink_latency_offset + u->source_latency_offset;
    else
        u->minimum_latency = 0;

    /* If the sink is valid, send a message to update the minimum latency to
     * the output thread, else set the variable directly */
    if (sink)
        pa_asyncmsgq_send(sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_UPDATE_MIN_LATENCY, NULL, u->minimum_latency, NULL);
    else
        u->output_thread_info.minimum_latency = u->minimum_latency;

    if (print_msg) {
        pa_log_info("Minimum possible end to end latency: %0.2f ms", (double)u->minimum_latency / PA_USEC_PER_MSEC);
        if (u->latency < u->minimum_latency)
            pa_log_warn("Configured latency of %0.2f ms is smaller than minimum latency, using minimum instead", (double)u->latency / PA_USEC_PER_MSEC);
    }
}

/* Called from main context */
static void adjust_rates(struct userdata *u) {
    size_t buffer;
    uint32_t old_rate, base_rate, new_rate, run_hours;
    int32_t latency_difference;
    pa_usec_t current_buffer_latency, snapshot_delay;
    int64_t current_source_sink_latency, current_latency, latency_at_optimum_rate;
    pa_usec_t final_latency, now, time_passed;

    pa_assert(u);
    pa_assert_ctl_context();

    /* Runtime and counters since last change of source or sink
     * or source/sink latency */
    run_hours = u->iteration_counter * u->real_adjust_time / PA_USEC_PER_SEC / 3600;
    u->iteration_counter +=1;

    /* If we are seeing underruns then the latency is too small */
    if (u->underrun_counter > 2) {
        pa_usec_t target_latency;

        target_latency = PA_MAX(u->latency, u->minimum_latency) + 5 * PA_USEC_PER_MSEC;

        if (u->max_latency == 0 || target_latency < u->max_latency) {
            u->underrun_latency_limit = PA_CLIP_SUB((int64_t)target_latency, u->sink_latency_offset + u->source_latency_offset);
            pa_log_warn("Too many underruns, increasing latency to %0.2f ms", (double)target_latency / PA_USEC_PER_MSEC);
        } else {
            u->underrun_latency_limit = PA_CLIP_SUB((int64_t)u->max_latency, u->sink_latency_offset + u->source_latency_offset);
            pa_log_warn("Too many underruns, configured maximum latency of %0.2f ms is reached", (double)u->max_latency / PA_USEC_PER_MSEC);
            pa_log_warn("Consider increasing the max_latency_msec");
        }

        update_minimum_latency(u, u->sink_input->sink, false);
        u->underrun_counter = 0;
    }

    /* Allow one underrun per hour */
    if (u->iteration_counter * u->real_adjust_time / PA_USEC_PER_SEC / 3600 > run_hours) {
        u->underrun_counter = PA_CLIP_SUB(u->underrun_counter, 1u);
        pa_log_info("Underrun counter: %u", u->underrun_counter);
    }

    /* Calculate real adjust time if source or sink did not change and if the system has
     * not been suspended. If the time between two calls is more than 5% longer than the
     * configured adjust time, we assume that the system has been sleeping and skip the
     * calculation for this iteration. */
    now = pa_rtclock_now();
    time_passed = now - u->adjust_time_stamp;
    if (!u->source_sink_changed && time_passed < u->adjust_time * 1.05) {
        u->adjust_counter++;
        u->real_adjust_time_sum += time_passed;
        u->real_adjust_time = u->real_adjust_time_sum / u->adjust_counter;
    }
    u->adjust_time_stamp = now;

    /* Rates and latencies */
    old_rate = u->sink_input->sample_spec.rate;
    base_rate = u->source_output->sample_spec.rate;

    buffer = u->latency_snapshot.loopback_memblockq_length;
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

    final_latency = PA_MAX(u->latency, u->minimum_latency);
    latency_difference = (int32_t)(latency_at_optimum_rate - final_latency);

    pa_log_debug("Loopback overall latency is %0.2f ms + %0.2f ms + %0.2f ms = %0.2f ms",
                (double) u->latency_snapshot.sink_latency / PA_USEC_PER_MSEC,
                (double) current_buffer_latency / PA_USEC_PER_MSEC,
                (double) u->latency_snapshot.source_latency / PA_USEC_PER_MSEC,
                (double) current_latency / PA_USEC_PER_MSEC);

    pa_log_debug("Loopback latency at base rate is %0.2f ms", (double)latency_at_optimum_rate / PA_USEC_PER_MSEC);

    /* Drop or insert samples if fast_adjust_threshold_msec was specified and the latency difference is too large. */
    if (u->fast_adjust_threshold > 0 && abs(latency_difference) > u->fast_adjust_threshold) {
        pa_log_debug ("Latency difference larger than %" PRIu64 " msec, skipping or inserting samples.", u->fast_adjust_threshold / PA_USEC_PER_MSEC);

        pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_FAST_ADJUST, NULL, current_source_sink_latency, NULL);

        /* Skip real adjust time calculation on next iteration. */
        u->source_sink_changed = true;
        return;
    }

    /* Calculate new rate */
    new_rate = rate_controller(base_rate, u->real_adjust_time, latency_difference);

    u->source_sink_changed = false;

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

        u->time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + 333 * PA_USEC_PER_MSEC, time_callback, u);
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

/* Called from main thread
 * Calculates minimum and maximum possible latency for source and sink */
static void update_latency_boundaries(struct userdata *u, pa_source *source, pa_sink *sink) {
    const char *s;

    if (source) {
        /* Source latencies */
        u->fixed_alsa_source = false;
        if (source->flags & PA_SOURCE_DYNAMIC_LATENCY)
            pa_source_get_latency_range(source, &u->min_source_latency, &u->max_source_latency);
        else {
            u->min_source_latency = pa_source_get_fixed_latency(source);
            u->max_source_latency = u->min_source_latency;
            if ((s = pa_proplist_gets(source->proplist, PA_PROP_DEVICE_API))) {
                if (pa_streq(s, "alsa"))
                    u->fixed_alsa_source = true;
            }
        }
        /* Source offset */
        u->source_latency_offset = source->port_latency_offset;

        /* Latencies below 2.5 ms cause problems, limit source latency if possible */
        if (u->max_source_latency >= MIN_DEVICE_LATENCY)
            u->min_source_latency = PA_MAX(u->min_source_latency, MIN_DEVICE_LATENCY);
        else
            u->min_source_latency = u->max_source_latency;
    }

    if (sink) {
        /* Sink latencies */
        if (sink->flags & PA_SINK_DYNAMIC_LATENCY)
            pa_sink_get_latency_range(sink, &u->min_sink_latency, &u->max_sink_latency);
        else {
            u->min_sink_latency = pa_sink_get_fixed_latency(sink);
            u->max_sink_latency = u->min_sink_latency;
        }
        /* Sink offset */
        u->sink_latency_offset = sink->port_latency_offset;

        /* Latencies below 2.5 ms cause problems, limit sink latency if possible */
        if (u->max_sink_latency >= MIN_DEVICE_LATENCY)
            u->min_sink_latency = PA_MAX(u->min_sink_latency, MIN_DEVICE_LATENCY);
        else
            u->min_sink_latency = u->max_sink_latency;
    }

    update_minimum_latency(u, sink, true);
}

/* Called from output context
 * Sets the memblockq to the configured latency corrected by latency_offset_usec */
static void memblockq_adjust(struct userdata *u, int64_t latency_offset_usec, bool allow_push) {
    size_t current_memblockq_length, requested_memblockq_length, buffer_correction;
    int64_t requested_buffer_latency;
    pa_usec_t final_latency, requested_sink_latency;

    final_latency = PA_MAX(u->latency, u->output_thread_info.minimum_latency);

    /* If source or sink have some large negative latency offset, we might want to
     * hold more than final_latency in the memblockq */
    requested_buffer_latency = (int64_t)final_latency - latency_offset_usec;

    /* Keep at least one sink latency in the queue to make sure that the sink
     * never underruns initially */
    requested_sink_latency = pa_sink_get_requested_latency_within_thread(u->sink_input->sink);
    if (requested_buffer_latency < (int64_t)requested_sink_latency)
        requested_buffer_latency = requested_sink_latency;

    requested_memblockq_length = pa_usec_to_bytes(requested_buffer_latency, &u->sink_input->sample_spec);
    current_memblockq_length = pa_memblockq_get_length(u->memblockq);

    if (current_memblockq_length > requested_memblockq_length) {
        /* Drop audio from queue */
        buffer_correction = current_memblockq_length - requested_memblockq_length;
        pa_log_info("Dropping %" PRIu64 " usec of audio from queue", pa_bytes_to_usec(buffer_correction, &u->sink_input->sample_spec));
        pa_memblockq_drop(u->memblockq, buffer_correction);

    } else if (current_memblockq_length < requested_memblockq_length && allow_push) {
        /* Add silence to queue */
        buffer_correction = requested_memblockq_length - current_memblockq_length;
        pa_log_info("Adding %" PRIu64 " usec of silence to queue", pa_bytes_to_usec(buffer_correction, &u->sink_input->sample_spec));
        pa_memblockq_seek(u->memblockq, (int64_t)buffer_correction, PA_SEEK_RELATIVE, true);
    }
}

/* Called from input thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct userdata *u;
    pa_usec_t push_time;
    int64_t current_source_latency;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert_se(u = o->userdata);

    /* Send current source latency and timestamp with the message */
    push_time = pa_rtclock_now();
    current_source_latency = pa_source_get_latency_within_thread(u->source_output->source, true);

    pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_POST, PA_INT_TO_PTR(current_source_latency), push_time, chunk, NULL);
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

/* Called from input thread context */
static int source_output_process_msg_cb(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE_OUTPUT(obj)->userdata;

    switch (code) {

        case SOURCE_OUTPUT_MESSAGE_LATENCY_SNAPSHOT: {
            size_t length;

            length = pa_memblockq_get_length(u->source_output->thread_info.delay_memblockq);

            u->latency_snapshot.send_counter = u->send_counter;
            /* Add content of delay memblockq to the source latency */
            u->latency_snapshot.source_latency = pa_source_get_latency_within_thread(u->source_output->source, true) +
                                                 pa_bytes_to_usec(length, &u->source_output->source->sample_spec);
            u->latency_snapshot.source_timestamp = pa_rtclock_now();

            return 0;
        }
    }

    return pa_source_output_process_msg(obj, code, data, offset, chunk);
}

/* Called from main thread.
 * Get current effective latency of the source. If the source is in use with
 * smaller latency than the configured latency, it will continue running with
 * the smaller value when the source output is switched to the source. */
static void update_effective_source_latency(struct userdata *u, pa_source *source, pa_sink *sink) {
    pa_usec_t effective_source_latency;

    effective_source_latency = u->configured_source_latency;

    if (source) {
        effective_source_latency = pa_source_get_requested_latency(source);
        if (effective_source_latency == 0 || effective_source_latency > u->configured_source_latency)
            effective_source_latency = u->configured_source_latency;
    }

    /* If the sink is valid, send a message to the output thread, else set the variable directly */
    if (sink)
        pa_asyncmsgq_send(sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_SET_EFFECTIVE_SOURCE_LATENCY, NULL, (int64_t)effective_source_latency, NULL);
    else
       u->output_thread_info.effective_source_latency = effective_source_latency;
}

/* Called from main thread.
 * Set source output latency to one third of the overall latency if possible.
 * The choice of one third is rather arbitrary somewhere between the minimum
 * possible latency which would cause a lot of CPU load and half the configured
 * latency which would quickly lead to underruns */
static void set_source_output_latency(struct userdata *u, pa_source *source) {
    pa_usec_t latency, requested_latency;

    requested_latency = u->latency / 3;

    /* Normally we try to configure sink and source latency equally. If the
     * sink latency cannot match the requested source latency try to set the
     * source latency to a smaller value to avoid underruns */
    if (u->min_sink_latency > requested_latency) {
        latency = PA_MAX(u->latency, u->minimum_latency);
        requested_latency = (latency - u->min_sink_latency) / 2;
    }

    latency = PA_CLAMP(requested_latency , u->min_source_latency, u->max_source_latency);
    u->configured_source_latency = pa_source_output_set_requested_latency(u->source_output, latency);
    if (u->configured_source_latency != requested_latency)
        pa_log_warn("Cannot set requested source latency of %0.2f ms, adjusting to %0.2f ms", (double)requested_latency / PA_USEC_PER_MSEC, (double)u->configured_source_latency / PA_USEC_PER_MSEC);
}

/* Called from input thread context */
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

/* Called from input thread context */
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

    /* Set latency and calculate latency limits */
    u->underrun_latency_limit = 0;
    update_latency_boundaries(u, dest, u->sink_input->sink);
    set_source_output_latency(u, dest);
    update_effective_source_latency(u, dest, u->sink_input->sink);

    /* Uncork the sink input unless the destination is suspended for other
     * reasons than idle. */
    if (dest->state == PA_SOURCE_SUSPENDED)
        pa_sink_input_cork(u->sink_input, (dest->suspend_cause != PA_SUSPEND_IDLE));
    else
        pa_sink_input_cork(u->sink_input, false);

    update_adjust_timer(u);

    /* Reset counters */
    u->iteration_counter = 0;
    u->underrun_counter = 0;

    u->source_sink_changed = true;

    /* Send a mesage to the output thread that the source has changed.
     * If the sink is invalid here during a profile switching situation
     * we can safely set push_called to false directly. */
    if (u->sink_input->sink)
        pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_SOURCE_CHANGED, NULL, 0, NULL);
    else
        u->output_thread_info.push_called = false;

    /* The sampling rate may be far away from the default rate if we are still
     * recovering from a previous source or sink change, so reset rate to
     * default before moving the source. */
    pa_sink_input_set_rate(u->sink_input, u->source_output->sample_spec.rate);
}

/* Called from main thread */
static void source_output_suspend_cb(pa_source_output *o, pa_source_state_t old_state, pa_suspend_cause_t old_suspend_cause) {
    struct userdata *u;
    bool suspended;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert_se(u = o->userdata);

    /* State has not changed, nothing to do */
    if (old_state == o->source->state)
        return;

    suspended = (o->source->state == PA_SOURCE_SUSPENDED);

    /* If the source has been suspended, we need to handle this like
     * a source change when the source is resumed */
    if (suspended) {
        if (u->sink_input->sink)
            pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input), SINK_INPUT_MESSAGE_SOURCE_CHANGED, NULL, 0, NULL);
        else
            u->output_thread_info.push_called = false;

    } else
        /* Get effective source latency on unsuspend */
        update_effective_source_latency(u, u->source_output->source, u->sink_input->sink);

    pa_sink_input_cork(u->sink_input, suspended);

    update_adjust_timer(u);
}

/* Called from input thread context */
static void update_source_latency_range_cb(pa_source_output *i) {
    struct userdata *u;

    pa_source_output_assert_ref(i);
    pa_source_output_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    /* Source latency may have changed */
    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), LOOPBACK_MESSAGE_SOURCE_LATENCY_RANGE_CHANGED, NULL, 0, NULL, NULL);
}

/* Called from output thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);
    pa_assert(chunk);

    /* It seems necessary to handle outstanding push messages here, though it is not clear
     * why. Removing this part leads to underruns when low latencies are configured. */
    u->output_thread_info.in_pop = true;
    while (pa_asyncmsgq_process_one(u->asyncmsgq) > 0)
        ;
    u->output_thread_info.in_pop = false;

    /* While pop has not been called, latency adjustments in SINK_INPUT_MESSAGE_POST are
     * enabled. Disable them on second pop and enable the final adjustment during the
     * next push. The adjustment must be done on the next push, because there is no way
     * to retrieve the source latency here. We are waiting for the second pop, because
     * the first pop may be called before the sink is actually started. */
    if (!u->output_thread_info.pop_called && u->output_thread_info.first_pop_done) {
        u->output_thread_info.pop_adjust = true;
        u->output_thread_info.pop_called = true;
    }
    u->output_thread_info.first_pop_done = true;

    if (pa_memblockq_peek(u->memblockq, chunk) < 0) {
        pa_log_info("Could not peek into queue");
        return -1;
    }

    chunk->length = PA_MIN(chunk->length, nbytes);
    pa_memblockq_drop(u->memblockq, chunk->length);

    /* Adjust the memblockq to ensure that there is
     * enough data in the queue to avoid underruns. */
    if (!u->output_thread_info.push_called)
        memblockq_adjust(u, 0, true);

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

    pa_sink_input_assert_io_context(u->sink_input);

    switch (code) {

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = data;

            *r = pa_bytes_to_usec(pa_memblockq_get_length(u->memblockq), &u->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
        }

        case SINK_INPUT_MESSAGE_POST:

            pa_memblockq_push_align(u->memblockq, chunk);

            /* If push has not been called yet, latency adjustments in sink_input_pop_cb()
             * are enabled. Disable them on first push and correct the memblockq. If pop
             * has not been called yet, wait until the pop_cb() requests the adjustment */
            if (u->output_thread_info.pop_called && (!u->output_thread_info.push_called || u->output_thread_info.pop_adjust)) {
                int64_t time_delta;

                /* This is the source latency at the time push was called */
                time_delta = PA_PTR_TO_INT(data);
                /* Add the time between push and post */
                time_delta += pa_rtclock_now() - (pa_usec_t) offset;
                /* Add the sink latency */
                time_delta += pa_sink_get_latency_within_thread(u->sink_input->sink, true);

                /* The source latency report includes the audio in the chunk,
                 * but since we already pushed the chunk to the memblockq, we need
                 * to subtract the chunk size from the source latency so that it
                 * won't be counted towards both the memblockq latency and the
                 * source latency.
                 *
                 * Sometimes the alsa source reports way too low latency (might
                 * be a bug in the alsa source code). This seems to happen when
                 * there's an overrun. As an attempt to detect overruns, we
                 * check if the chunk size is larger than the configured source
                 * latency. If so, we assume that the source should have pushed
                 * a chunk whose size equals the configured latency, so we
                 * modify time_delta only by that amount, which makes
                 * memblockq_adjust() drop more data than it would otherwise.
                 * This seems to work quite well, but it's possible that the
                 * next push also contains too much data, and in that case the
                 * resulting latency will be wrong. */
                if (pa_bytes_to_usec(chunk->length, &u->sink_input->sample_spec) > u->output_thread_info.effective_source_latency)
                    time_delta -= (int64_t)u->output_thread_info.effective_source_latency;
                else
                    time_delta -= (int64_t)pa_bytes_to_usec(chunk->length, &u->sink_input->sample_spec);

                /* FIXME: We allow pushing silence here to fix up the latency. This
                 * might lead to a gap in the stream */
                memblockq_adjust(u, time_delta, true);

                u->output_thread_info.pop_adjust = false;
                u->output_thread_info.push_called = true;
            }

            /* If pop has not been called yet, make sure the latency does not grow too much.
             * Don't push any silence here, because we already have new data in the queue */
            if (!u->output_thread_info.pop_called)
                 memblockq_adjust(u, 0, false);

            /* Is this the end of an underrun? Then let's start things
             * right-away */
            if (u->sink_input->sink->thread_info.state != PA_SINK_SUSPENDED &&
                u->sink_input->thread_info.underrun_for > 0 &&
                pa_memblockq_is_readable(u->memblockq)) {

                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), LOOPBACK_MESSAGE_UNDERRUN, NULL, 0, NULL, NULL);
                /* If called from within the pop callback skip the rewind */
                if (!u->output_thread_info.in_pop) {
                    pa_log_debug("Requesting rewind due to end of underrun.");
                    pa_sink_input_request_rewind(u->sink_input,
                                                 (size_t) (u->sink_input->thread_info.underrun_for == (size_t) -1 ? 0 : u->sink_input->thread_info.underrun_for),
                                                 false, true, false);
                }
            }

            u->output_thread_info.recv_counter += (int64_t) chunk->length;

            return 0;

        case SINK_INPUT_MESSAGE_REWIND:

            /* Do not try to rewind if no data was pushed yet */
            if (u->output_thread_info.push_called)
                pa_memblockq_seek(u->memblockq, -offset, PA_SEEK_RELATIVE, true);

            u->output_thread_info.recv_counter -= offset;

            return 0;

        case SINK_INPUT_MESSAGE_LATENCY_SNAPSHOT: {
            size_t length;

            length = pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq);

            u->latency_snapshot.recv_counter = u->output_thread_info.recv_counter;
            u->latency_snapshot.loopback_memblockq_length = pa_memblockq_get_length(u->memblockq);
            /* Add content of render memblockq to sink latency */
            u->latency_snapshot.sink_latency = pa_sink_get_latency_within_thread(u->sink_input->sink, true) +
                                               pa_bytes_to_usec(length, &u->sink_input->sink->sample_spec);
            u->latency_snapshot.sink_timestamp = pa_rtclock_now();

            return 0;
        }

        case SINK_INPUT_MESSAGE_SOURCE_CHANGED:

            u->output_thread_info.push_called = false;

            return 0;

        case SINK_INPUT_MESSAGE_SET_EFFECTIVE_SOURCE_LATENCY:

            u->output_thread_info.effective_source_latency = (pa_usec_t)offset;

            return 0;

        case SINK_INPUT_MESSAGE_UPDATE_MIN_LATENCY:

            u->output_thread_info.minimum_latency = (pa_usec_t)offset;

            return 0;

        case SINK_INPUT_MESSAGE_FAST_ADJUST:

            memblockq_adjust(u, offset, true);

            return 0;
    }

    return pa_sink_input_process_msg(obj, code, data, offset, chunk);
}
/* Called from main thread.
 * Set sink input latency to one third of the overall latency if possible.
 * The choice of one third is rather arbitrary somewhere between the minimum
 * possible latency which would cause a lot of CPU load and half the configured
 * latency which would quickly lead to underruns. */
static void set_sink_input_latency(struct userdata *u, pa_sink *sink) {
     pa_usec_t latency, requested_latency;

    requested_latency = u->latency / 3;

    /* Normally we try to configure sink and source latency equally. If the
     * source latency cannot match the requested sink latency try to set the
     * sink latency to a smaller value to avoid underruns */
    if (u->min_source_latency > requested_latency) {
        latency = PA_MAX(u->latency, u->minimum_latency);
        requested_latency = (latency - u->min_source_latency) / 2;
    }

    latency = PA_CLAMP(requested_latency , u->min_sink_latency, u->max_sink_latency);
    u->configured_sink_latency = pa_sink_input_set_requested_latency(u->sink_input, latency);
    if (u->configured_sink_latency != requested_latency)
        pa_log_warn("Cannot set requested sink latency of %0.2f ms, adjusting to %0.2f ms", (double)requested_latency / PA_USEC_PER_MSEC, (double)u->configured_sink_latency / PA_USEC_PER_MSEC);
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

    /* Set latency and calculate latency limits */
    u->underrun_latency_limit = 0;
    update_latency_boundaries(u, NULL, dest);
    set_sink_input_latency(u, dest);
    update_effective_source_latency(u, u->source_output->source, dest);

    /* Uncork the source output unless the destination is suspended for other
     * reasons than idle */
    if (dest->state == PA_SINK_SUSPENDED)
        pa_source_output_cork(u->source_output, (dest->suspend_cause != PA_SUSPEND_IDLE));
    else
        pa_source_output_cork(u->source_output, false);

    update_adjust_timer(u);

    /* Reset counters */
    u->iteration_counter = 0;
    u->underrun_counter = 0;

    u->source_sink_changed = true;

    u->output_thread_info.pop_called = false;
    u->output_thread_info.first_pop_done = false;

    /* Sample rate may be far away from the default rate if we are still
     * recovering from a previous source or sink change, so reset rate to
     * default before moving the sink. */
    pa_sink_input_set_rate(u->sink_input, u->source_output->sample_spec.rate);
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
static void sink_input_suspend_cb(pa_sink_input *i, pa_sink_state_t old_state, pa_suspend_cause_t old_suspend_cause) {
    struct userdata *u;
    bool suspended;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert_se(u = i->userdata);

    /* State has not changed, nothing to do */
    if (old_state == i->sink->state)
        return;

    suspended = (i->sink->state == PA_SINK_SUSPENDED);

    /* If the sink has been suspended, we need to handle this like
     * a sink change when the sink is resumed. Because the sink
     * is suspended, we can set the variables directly. */
    if (suspended) {
        u->output_thread_info.pop_called = false;
        u->output_thread_info.first_pop_done = false;

    } else
        /* Set effective source latency on unsuspend */
        update_effective_source_latency(u, u->source_output->source, u->sink_input->sink);

    pa_source_output_cork(u->source_output, suspended);

    update_adjust_timer(u);
}

/* Called from output thread context */
static void update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert_se(u = i->userdata);

    /* Sink latency may have changed */
    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), LOOPBACK_MESSAGE_SINK_LATENCY_RANGE_CHANGED, NULL, 0, NULL, NULL);
}

/* Called from main context */
static int loopback_process_msg_cb(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    struct loopback_msg *msg;
    struct userdata *u;
    pa_usec_t current_latency;

    pa_assert(o);
    pa_assert_ctl_context();

    msg = LOOPBACK_MSG(o);
    pa_assert_se(u = msg->userdata);

    switch (code) {

        case LOOPBACK_MESSAGE_SOURCE_LATENCY_RANGE_CHANGED:

            update_effective_source_latency(u, u->source_output->source, u->sink_input->sink);
            current_latency = pa_source_get_requested_latency(u->source_output->source);
            if (current_latency > u->configured_source_latency) {
                /* The minimum latency has changed to a value larger than the configured latency, so
                 * the source latency has been increased. The case that the minimum latency changes
                 * back to a smaller value is not handled because this never happens with the current
                 * source implementations. */
                pa_log_warn("Source minimum latency increased to %0.2f ms", (double)current_latency / PA_USEC_PER_MSEC);
                u->configured_source_latency = current_latency;
                update_latency_boundaries(u, u->source_output->source, u->sink_input->sink);
                /* We re-start counting when the latency has changed */
                u->iteration_counter = 0;
                u->underrun_counter = 0;
            }

            return 0;

        case LOOPBACK_MESSAGE_SINK_LATENCY_RANGE_CHANGED:

            current_latency = pa_sink_get_requested_latency(u->sink_input->sink);
            if (current_latency > u->configured_sink_latency) {
                /* The minimum latency has changed to a value larger than the configured latency, so
                 * the sink latency has been increased. The case that the minimum latency changes back
                 * to a smaller value is not handled because this never happens with the current sink
                 * implementations. */
                pa_log_warn("Sink minimum latency increased to %0.2f ms", (double)current_latency / PA_USEC_PER_MSEC);
                u->configured_sink_latency = current_latency;
                update_latency_boundaries(u, u->source_output->source, u->sink_input->sink);
                /* We re-start counting when the latency has changed */
                u->iteration_counter = 0;
                u->underrun_counter = 0;
            }

            return 0;

        case LOOPBACK_MESSAGE_UNDERRUN:

            u->underrun_counter++;
            pa_log_debug("Underrun detected, counter incremented to %u", u->underrun_counter);

            return 0;

    }

    return 0;
}

static pa_hook_result_t sink_port_latency_offset_changed_cb(pa_core *core, pa_sink *sink, struct userdata *u) {

    if (sink != u->sink_input->sink)
        return PA_HOOK_OK;

    u->sink_latency_offset = sink->port_latency_offset;
    update_minimum_latency(u, sink, true);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_port_latency_offset_changed_cb(pa_core *core, pa_source *source, struct userdata *u) {

    if (source != u->source_output->source)
        return PA_HOOK_OK;

    u->source_latency_offset = source->port_latency_offset;
    update_minimum_latency(u, u->sink_input->sink, true);

    return PA_HOOK_OK;
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
    uint32_t max_latency_msec;
    uint32_t fast_adjust_threshold;
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

    if (source) {
        ss = source->sample_spec;
        map = source->channel_map;
        format_set = true;
        rate_set = true;
        channels_set = true;
    } else if (sink) {
        ss = sink->sample_spec;
        map = sink->channel_map;
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

    fast_adjust_threshold = 0;
    if (pa_modargs_get_value_u32(ma, "fast_adjust_threshold_msec", &fast_adjust_threshold) < 0 || (fast_adjust_threshold != 0 && fast_adjust_threshold < 100)) {
        pa_log("Invalid fast adjust threshold specification");
        goto fail;
    }

    max_latency_msec = 0;
    if (pa_modargs_get_value_u32(ma, "max_latency_msec", &max_latency_msec) < 0) {
        pa_log("Invalid maximum latency specification");
        goto fail;
    }

    if (max_latency_msec > 0 && max_latency_msec < latency_msec) {
        pa_log_warn("Configured maximum latency is smaller than latency, using latency instead");
        max_latency_msec = latency_msec;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->latency = (pa_usec_t) latency_msec * PA_USEC_PER_MSEC;
    u->max_latency = (pa_usec_t) max_latency_msec * PA_USEC_PER_MSEC;
    u->output_thread_info.pop_called = false;
    u->output_thread_info.pop_adjust = false;
    u->output_thread_info.push_called = false;
    u->iteration_counter = 0;
    u->underrun_counter = 0;
    u->underrun_latency_limit = 0;
    u->source_sink_changed = true;
    u->real_adjust_time_sum = 0;
    u->adjust_counter = 0;
    u->fast_adjust_threshold = fast_adjust_threshold * PA_USEC_PER_MSEC;

    adjust_time_sec = DEFAULT_ADJUST_TIME_USEC / PA_USEC_PER_SEC;
    if (pa_modargs_get_value_u32(ma, "adjust_time", &adjust_time_sec) < 0) {
        pa_log("Failed to parse adjust_time value");
        goto fail;
    }

    if (adjust_time_sec != DEFAULT_ADJUST_TIME_USEC / PA_USEC_PER_SEC)
        u->adjust_time = adjust_time_sec * PA_USEC_PER_SEC;
    else
        u->adjust_time = DEFAULT_ADJUST_TIME_USEC;

    u->real_adjust_time = u->adjust_time;

    pa_source_output_new_data_init(&source_output_data);
    source_output_data.driver = __FILE__;
    source_output_data.module = m;
    if (source)
        pa_source_output_new_data_set_source(&source_output_data, source, false, true);

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

    if (!format_set)
        source_output_data.flags |= PA_SOURCE_OUTPUT_FIX_FORMAT;

    if (!rate_set)
        source_output_data.flags |= PA_SOURCE_OUTPUT_FIX_RATE;

    if (!channels_set)
        source_output_data.flags |= PA_SOURCE_OUTPUT_FIX_CHANNELS;

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
    u->source_output->may_move_to = source_output_may_move_to_cb;
    u->source_output->moving = source_output_moving_cb;
    u->source_output->suspend = source_output_suspend_cb;
    u->source_output->update_source_latency_range = update_source_latency_range_cb;
    u->source_output->update_source_fixed_latency = update_source_latency_range_cb;
    u->source_output->userdata = u;

    /* If format, rate or channels were originally unset, they are set now
     * after the pa_source_output_new() call. */
    ss = u->source_output->sample_spec;
    map = u->source_output->channel_map;

    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;

    if (sink)
        pa_sink_input_new_data_set_sink(&sink_input_data, sink, false, true);

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
    u->sink_input->update_sink_latency_range = update_sink_latency_range_cb;
    u->sink_input->update_sink_fixed_latency = update_sink_latency_range_cb;
    u->sink_input->userdata = u;

    update_latency_boundaries(u, u->source_output->source, u->sink_input->sink);
    set_sink_input_latency(u, u->sink_input->sink);
    set_source_output_latency(u, u->source_output->source);

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
    /* Fill the memblockq with silence */
    pa_memblockq_seek(u->memblockq, pa_usec_to_bytes(u->latency, &u->sink_input->sample_spec), PA_SEEK_RELATIVE, true);

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

    /* Hooks to track changes of latency offsets */
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_PORT_LATENCY_OFFSET_CHANGED],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) sink_port_latency_offset_changed_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_PORT_LATENCY_OFFSET_CHANGED],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) source_port_latency_offset_changed_cb, u);

    /* Setup message handler for main thread */
    u->msg = pa_msgobject_new(loopback_msg);
    u->msg->parent.process_msg = loopback_process_msg_cb;
    u->msg->userdata = u;

    /* The output thread is not yet running, set effective_source_latency directly */
    update_effective_source_latency(u, u->source_output->source, NULL);

    pa_sink_input_put(u->sink_input);
    pa_source_output_put(u->source_output);

    if (u->source_output->source->state != PA_SOURCE_SUSPENDED)
        pa_sink_input_cork(u->sink_input, false);

    if (u->sink_input->sink->state != PA_SINK_SUSPENDED)
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
