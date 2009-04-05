/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

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
#include <errno.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>
#include <pulsecore/module.h>
#include <pulsecore/llist.h>
#include <pulsecore/sink.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/mutex.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/core-error.h>
#include <pulsecore/time-smoother.h>

#include "module-combine-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Combine multiple sinks to one");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "slaves=<slave sinks> "
        "adjust_time=<seconds> "
        "resample_method=<method> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>");

#define DEFAULT_SINK_NAME "combined"

#define MEMBLOCKQ_MAXLENGTH (1024*1024*16)

#define DEFAULT_ADJUST_TIME 10

#define BLOCK_USEC (PA_USEC_PER_MSEC * 200)

static const char* const valid_modargs[] = {
    "sink_name",
    "slaves",
    "adjust_time",
    "resample_method",
    "format",
    "channels",
    "rate",
    "channel_map",
    NULL
};

struct output {
    struct userdata *userdata;

    pa_sink *sink;
    pa_sink_input *sink_input;

    pa_asyncmsgq *inq,    /* Message queue from the sink thread to this sink input */
                 *outq;   /* Message queue from this sink input to the sink thread */
    pa_rtpoll_item *inq_rtpoll_item_read, *inq_rtpoll_item_write;
    pa_rtpoll_item *outq_rtpoll_item_read, *outq_rtpoll_item_write;

    pa_memblockq *memblockq;

    pa_usec_t total_latency;

    pa_atomic_t max_request;

    PA_LLIST_FIELDS(struct output);
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_time_event *time_event;
    uint32_t adjust_time;

    pa_bool_t automatic;

    pa_hook_slot *sink_put_slot, *sink_unlink_slot, *sink_state_changed_slot;

    pa_resample_method_t resample_method;

    struct timeval adjust_timestamp;

    pa_usec_t block_usec;

    pa_idxset* outputs; /* managed in main context */

    struct {
        PA_LLIST_HEAD(struct output, active_outputs); /* managed in IO thread context */
        pa_atomic_t running;  /* we cache that value here, so that every thread can query it cheaply */
        pa_usec_t timestamp;
        pa_bool_t in_null_mode;
        pa_smoother *smoother;
        uint64_t counter;
    } thread_info;
};

enum {
    SINK_MESSAGE_ADD_OUTPUT = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_REMOVE_OUTPUT,
    SINK_MESSAGE_NEED,
    SINK_MESSAGE_UPDATE_LATENCY,
    SINK_MESSAGE_UPDATE_MAX_REQUEST
};

enum {
    SINK_INPUT_MESSAGE_POST = PA_SINK_INPUT_MESSAGE_MAX,
};

static void output_free(struct output *o);
static int output_create_sink_input(struct output *o);

static void adjust_rates(struct userdata *u) {
    struct output *o;
    pa_usec_t max_sink_latency = 0, min_total_latency = (pa_usec_t) -1, target_latency, avg_total_latency = 0;
    uint32_t base_rate;
    uint32_t idx;
    unsigned n = 0;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (pa_idxset_size(u->outputs) <= 0)
        return;

    if (!PA_SINK_IS_OPENED(pa_sink_get_state(u->sink)))
        return;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        pa_usec_t sink_latency;

        if (!o->sink_input || !PA_SINK_IS_OPENED(pa_sink_get_state(o->sink)))
            continue;

        o->total_latency = pa_sink_input_get_latency(o->sink_input, &sink_latency);
        o->total_latency += sink_latency;

        if (sink_latency > max_sink_latency)
            max_sink_latency = sink_latency;

        if (min_total_latency == (pa_usec_t) -1 || o->total_latency < min_total_latency)
            min_total_latency = o->total_latency;

        avg_total_latency += o->total_latency;
        n++;
    }

    if (min_total_latency == (pa_usec_t) -1)
        return;

    avg_total_latency /= n;

    target_latency = max_sink_latency > min_total_latency ? max_sink_latency : min_total_latency;

    pa_log_info("[%s] avg total latency is %0.2f msec.", u->sink->name, (double) avg_total_latency / PA_USEC_PER_MSEC);
    pa_log_info("[%s] target latency is %0.2f msec.", u->sink->name, (double) target_latency / PA_USEC_PER_MSEC);

    base_rate = u->sink->sample_spec.rate;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        uint32_t r = base_rate;

        if (!o->sink_input || !PA_SINK_IS_OPENED(pa_sink_get_state(o->sink)))
            continue;

        if (o->total_latency < target_latency)
            r -= (uint32_t) ((((double) (target_latency - o->total_latency))/(double)u->adjust_time)*(double)r/PA_USEC_PER_SEC);
        else if (o->total_latency > target_latency)
            r += (uint32_t) ((((double) (o->total_latency - target_latency))/(double)u->adjust_time)*(double)r/PA_USEC_PER_SEC);

        if (r < (uint32_t) (base_rate*0.9) || r > (uint32_t) (base_rate*1.1)) {
            pa_log_warn("[%s] sample rates too different, not adjusting (%u vs. %u).", pa_proplist_gets(o->sink_input->proplist, PA_PROP_MEDIA_NAME), base_rate, r);
            pa_sink_input_set_rate(o->sink_input, base_rate);
        } else {
            pa_log_info("[%s] new rate is %u Hz; ratio is %0.3f; latency is %0.0f usec.", pa_proplist_gets(o->sink_input->proplist, PA_PROP_MEDIA_NAME), r, (double) r / base_rate, (float) o->total_latency);
            pa_sink_input_set_rate(o->sink_input, r);
        }
    }

    pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_UPDATE_LATENCY, NULL, (int64_t) avg_total_latency, NULL);
}

static void time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval n;

    pa_assert(u);
    pa_assert(a);
    pa_assert(u->time_event == e);

    adjust_rates(u);

    pa_gettimeofday(&n);
    n.tv_sec += (time_t) u->adjust_time;
    u->sink->core->mainloop->time_restart(e, &n);
}

static void process_render_null(struct userdata *u, pa_usec_t now) {
    size_t ate = 0;
    pa_assert(u);

    if (u->thread_info.in_null_mode)
        u->thread_info.timestamp = now;

    while (u->thread_info.timestamp < now + u->block_usec) {
        pa_memchunk chunk;

        pa_sink_render(u->sink, u->sink->thread_info.max_request, &chunk);
        pa_memblock_unref(chunk.memblock);

        u->thread_info.counter += chunk.length;

/*         pa_log_debug("Ate %lu bytes.", (unsigned long) chunk.length); */
        u->thread_info.timestamp += pa_bytes_to_usec(chunk.length, &u->sink->sample_spec);

        ate += chunk.length;

        if (ate >= u->sink->thread_info.max_request)
            break;
    }

/*     pa_log_debug("Ate in sum %lu bytes (of %lu)", (unsigned long) ate, (unsigned long) nbytes); */

    pa_smoother_put(u->thread_info.smoother, now,
                    pa_bytes_to_usec(u->thread_info.counter, &u->sink->sample_spec) - (u->thread_info.timestamp - now));
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority+1);

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    u->thread_info.timestamp = pa_rtclock_usec();
    u->thread_info.in_null_mode = FALSE;

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            if (u->sink->thread_info.rewind_requested)
                pa_sink_process_rewind(u->sink, 0);

        /* If no outputs are connected, render some data and drop it immediately. */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state) && !u->thread_info.active_outputs) {
            pa_usec_t now;

            now = pa_rtclock_usec();

            if (!u->thread_info.in_null_mode || u->thread_info.timestamp <= now)
                process_render_null(u, now);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->thread_info.timestamp);
            u->thread_info.in_null_mode = TRUE;
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
            u->thread_info.in_null_mode = FALSE;
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0) {
            pa_log_info("pa_rtpoll_run() = %i", ret);
            goto fail;
        }

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

/* Called from I/O thread context */
static void render_memblock(struct userdata *u, struct output *o, size_t length) {
    pa_assert(u);
    pa_assert(o);

    /* We are run by the sink thread, on behalf of an output (o). The
     * output is waiting for us, hence it is safe to access its
     * mainblockq and asyncmsgq directly. */

    /* If we are not running, we cannot produce any data */
    if (!pa_atomic_load(&u->thread_info.running))
        return;

    /* Maybe there's some data in the requesting output's queue
     * now? */
    while (pa_asyncmsgq_process_one(o->inq) > 0)
        ;

    /* Ok, now let's prepare some data if we really have to */
    while (!pa_memblockq_is_readable(o->memblockq)) {
        struct output *j;
        pa_memchunk chunk;

        /* Render data! */
        pa_sink_render(u->sink, length, &chunk);

        u->thread_info.counter += chunk.length;

        /* OK, let's send this data to the other threads */
        for (j = u->thread_info.active_outputs; j; j = j->next)

            /* Send to other outputs, which are not the requesting
             * one */

            if (j != o)
                pa_asyncmsgq_post(j->inq, PA_MSGOBJECT(j->sink_input), SINK_INPUT_MESSAGE_POST, NULL, 0, &chunk, NULL);

        /* And place it directly into the requesting output's queue */
        if (o)
            pa_memblockq_push_align(o->memblockq, &chunk);

        pa_memblock_unref(chunk.memblock);
    }
}

/* Called from I/O thread context */
static void request_memblock(struct output *o, size_t length) {
    pa_assert(o);
    pa_sink_input_assert_ref(o->sink_input);
    pa_sink_assert_ref(o->userdata->sink);

    /* If another thread already prepared some data we received
     * the data over the asyncmsgq, hence let's first process
     * it. */
    while (pa_asyncmsgq_process_one(o->inq) > 0)
        ;

    /* Check whether we're now readable */
    if (pa_memblockq_is_readable(o->memblockq))
        return;

    /* OK, we need to prepare new data, but only if the sink is actually running */
    if (pa_atomic_load(&o->userdata->thread_info.running))
        pa_asyncmsgq_send(o->outq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_NEED, o, (int64_t) length, NULL);
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    /* If necessary, get some new data */
    request_memblock(o, nbytes);

    if (pa_memblockq_peek(o->memblockq, chunk) < 0)
        return -1;

    pa_memblockq_drop(o->memblockq, chunk->length);
    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    pa_memblockq_rewind(o->memblockq, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    pa_memblockq_set_maxrewind(o->memblockq, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    if (pa_atomic_load(&o->max_request) == (int) nbytes)
        return;

    pa_atomic_store(&o->max_request, (int) nbytes);

    pa_asyncmsgq_post(o->outq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_UPDATE_MAX_REQUEST, NULL, 0, NULL, NULL);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    /* Set up the queue from the sink thread to us */
    pa_assert(!o->inq_rtpoll_item_read && !o->outq_rtpoll_item_write);

    o->inq_rtpoll_item_read = pa_rtpoll_item_new_asyncmsgq_read(
            i->sink->rtpoll,
            PA_RTPOLL_LATE,  /* This one is not that important, since we check for data in _peek() anyway. */
            o->inq);

    o->outq_rtpoll_item_write = pa_rtpoll_item_new_asyncmsgq_write(
            i->sink->rtpoll,
            PA_RTPOLL_EARLY,
            o->outq);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    /* Shut down the queue from the sink thread to us */
    pa_assert(o->inq_rtpoll_item_read && o->outq_rtpoll_item_write);

    pa_rtpoll_item_free(o->inq_rtpoll_item_read);
    o->inq_rtpoll_item_read = NULL;

    pa_rtpoll_item_free(o->outq_rtpoll_item_write);
    o->outq_rtpoll_item_write = NULL;
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    pa_module_unload_request(o->userdata->module, TRUE);
    output_free(o);
}

/* Called from IO thread context */
static void sink_input_state_change_cb(pa_sink_input *i, pa_sink_input_state_t state) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* If we are added for the first time, ask for a rewinding so that
     * we are heard right-away. */
    if (PA_SINK_INPUT_IS_LINKED(state) &&
        i->thread_info.state == PA_SINK_INPUT_INIT)
        pa_sink_input_request_rewind(i, 0, FALSE, TRUE, TRUE);
}

/* Called from thread context */
static int sink_input_process_msg(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct output *o = PA_SINK_INPUT(obj)->userdata;

    switch (code) {

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
             pa_usec_t *r = data;

            *r = pa_bytes_to_usec(pa_memblockq_get_length(o->memblockq), &o->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
        }

        case SINK_INPUT_MESSAGE_POST:

            if (PA_SINK_IS_OPENED(o->sink_input->sink->thread_info.state))
                pa_memblockq_push_align(o->memblockq, chunk);
            else
                pa_memblockq_flush_write(o->memblockq);

            return 0;
    }

    return pa_sink_input_process_msg(obj, code, data, offset, chunk);
}

/* Called from main context */
static void disable_output(struct output *o) {
    pa_assert(o);

    if (!o->sink_input)
        return;

    pa_sink_input_unlink(o->sink_input);
    pa_asyncmsgq_send(o->userdata->sink->asyncmsgq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_REMOVE_OUTPUT, o, 0, NULL);
    pa_sink_input_unref(o->sink_input);
    o->sink_input = NULL;
}

/* Called from main context */
static void enable_output(struct output *o) {
    pa_assert(o);

    if (o->sink_input)
        return;

    if (output_create_sink_input(o) >= 0) {

        pa_memblockq_flush_write(o->memblockq);

        pa_sink_input_put(o->sink_input);

        if (o->userdata->sink && PA_SINK_IS_LINKED(pa_sink_get_state(o->userdata->sink)))
            pa_asyncmsgq_send(o->userdata->sink->asyncmsgq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_ADD_OUTPUT, o, 0, NULL);
    }
}

/* Called from main context */
static void suspend(struct userdata *u) {
    struct output *o;
    uint32_t idx;

    pa_assert(u);

    /* Let's suspend by unlinking all streams */
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        disable_output(o);

    pa_log_info("Device suspended...");
}

/* Called from main context */
static void unsuspend(struct userdata *u) {
    struct output *o;
    uint32_t idx;

    pa_assert(u);

    /* Let's resume */
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {

        pa_sink_suspend(o->sink, FALSE);

        if (PA_SINK_IS_OPENED(pa_sink_get_state(o->sink)))
            enable_output(o);
    }

    pa_log_info("Resumed successfully...");
}

/* Called from main context */
static int sink_set_state(pa_sink *sink, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(sink);
    pa_assert_se(u = sink->userdata);

    /* Please note that in contrast to the ALSA modules we call
     * suspend/unsuspend from main context here! */

    switch (state) {
        case PA_SINK_SUSPENDED:
            pa_assert(PA_SINK_IS_OPENED(pa_sink_get_state(u->sink)));

            suspend(u);
            break;

        case PA_SINK_IDLE:
        case PA_SINK_RUNNING:

            if (pa_sink_get_state(u->sink) == PA_SINK_SUSPENDED)
                unsuspend(u);

            break;

        case PA_SINK_UNLINKED:
        case PA_SINK_INIT:
        case PA_SINK_INVALID_STATE:
            ;
    }

    return 0;
}

/* Called from IO context */
static void update_max_request(struct userdata *u) {
    size_t max_request = 0;
    struct output *o;

    for (o = u->thread_info.active_outputs; o; o = o->next) {
        size_t mr = (size_t) pa_atomic_load(&o->max_request);

        if (mr > max_request)
            max_request = mr;
    }

    if (max_request <= 0)
        max_request = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);

    pa_sink_set_max_request_within_thread(u->sink, max_request);
}

/* Called from thread context of the io thread */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_SET_STATE:
            pa_atomic_store(&u->thread_info.running, PA_PTR_TO_UINT(data) == PA_SINK_RUNNING);

            if (PA_PTR_TO_UINT(data) == PA_SINK_SUSPENDED)
                pa_smoother_pause(u->thread_info.smoother, pa_rtclock_usec());
            else
                pa_smoother_resume(u->thread_info.smoother, pa_rtclock_usec(), TRUE);

            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t x, y, c, *delay = data;

            x = pa_rtclock_usec();
            y = pa_smoother_get(u->thread_info.smoother, x);

            c = pa_bytes_to_usec(u->thread_info.counter, &u->sink->sample_spec);

            if (y < c)
                *delay = c - y;
            else
                *delay = 0;

            return 0;
        }

        case SINK_MESSAGE_ADD_OUTPUT: {
            struct output *op = data;

            PA_LLIST_PREPEND(struct output, u->thread_info.active_outputs, op);

            pa_assert(!op->outq_rtpoll_item_read && !op->inq_rtpoll_item_write);

            op->outq_rtpoll_item_read = pa_rtpoll_item_new_asyncmsgq_read(
                    u->rtpoll,
                    PA_RTPOLL_EARLY-1,  /* This item is very important */
                    op->outq);
            op->inq_rtpoll_item_write = pa_rtpoll_item_new_asyncmsgq_write(
                    u->rtpoll,
                    PA_RTPOLL_EARLY,
                    op->inq);

            update_max_request(u);
            return 0;
        }

        case SINK_MESSAGE_REMOVE_OUTPUT: {
            struct output *op = data;

            PA_LLIST_REMOVE(struct output, u->thread_info.active_outputs, op);

            pa_assert(op->outq_rtpoll_item_read && op->inq_rtpoll_item_write);

            pa_rtpoll_item_free(op->outq_rtpoll_item_read);
            op->outq_rtpoll_item_read = NULL;

            pa_rtpoll_item_free(op->inq_rtpoll_item_write);
            op->inq_rtpoll_item_write = NULL;

            update_max_request(u);
            return 0;
        }

        case SINK_MESSAGE_NEED:
            render_memblock(u, (struct output*) data, (size_t) offset);
            return 0;

        case SINK_MESSAGE_UPDATE_LATENCY: {
            pa_usec_t x, y, latency = (pa_usec_t) offset;

            x = pa_rtclock_usec();
            y = pa_bytes_to_usec(u->thread_info.counter, &u->sink->sample_spec);

            if (y > latency)
                y -= latency;
            else
                y = 0;

            pa_smoother_put(u->thread_info.smoother, x, y);
            return 0;
        }

        case SINK_MESSAGE_UPDATE_MAX_REQUEST:

            update_max_request(u);
            break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void update_description(struct userdata *u) {
    pa_bool_t first = TRUE;
    char *t;
    struct output *o;
    uint32_t idx;

    pa_assert(u);

    if (pa_idxset_isempty(u->outputs)) {
        pa_sink_set_description(u->sink, "Simultaneous output");
        return;
    }

    t = pa_xstrdup("Simultaneous output to");

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        char *e;

        if (first) {
            e = pa_sprintf_malloc("%s %s", t, pa_strnull(pa_proplist_gets(o->sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));
            first = FALSE;
        } else
            e = pa_sprintf_malloc("%s, %s", t, pa_strnull(pa_proplist_gets(o->sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));

        pa_xfree(t);
        t = e;
    }

    pa_sink_set_description(u->sink, t);
    pa_xfree(t);
}

static int output_create_sink_input(struct output *o) {
    pa_sink_input_new_data data;

    pa_assert(o);

    if (o->sink_input)
        return 0;

    pa_sink_input_new_data_init(&data);
    data.sink = o->sink;
    data.driver = __FILE__;
    pa_proplist_setf(data.proplist, PA_PROP_MEDIA_NAME, "Simultaneous output on %s", pa_strnull(pa_proplist_gets(o->sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));
    pa_proplist_sets(data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&data, &o->userdata->sink->sample_spec);
    pa_sink_input_new_data_set_channel_map(&data, &o->userdata->sink->channel_map);
    data.module = o->userdata->module;
    data.resample_method = o->userdata->resample_method;

    pa_sink_input_new(&o->sink_input, o->userdata->core, &data, PA_SINK_INPUT_VARIABLE_RATE|PA_SINK_INPUT_DONT_MOVE);

    pa_sink_input_new_data_done(&data);

    if (!o->sink_input)
        return -1;

    o->sink_input->parent.process_msg = sink_input_process_msg;
    o->sink_input->pop = sink_input_pop_cb;
    o->sink_input->process_rewind = sink_input_process_rewind_cb;
    o->sink_input->state_change = sink_input_state_change_cb;
    o->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    o->sink_input->update_max_request = sink_input_update_max_request_cb;
    o->sink_input->attach = sink_input_attach_cb;
    o->sink_input->detach = sink_input_detach_cb;
    o->sink_input->kill = sink_input_kill_cb;
    o->sink_input->userdata = o;

    pa_sink_input_set_requested_latency(o->sink_input, BLOCK_USEC);

    return 0;
}

static struct output *output_new(struct userdata *u, pa_sink *sink) {
    struct output *o;
    pa_sink_state_t state;

    pa_assert(u);
    pa_assert(sink);
    pa_assert(u->sink);

    o = pa_xnew(struct output, 1);
    o->userdata = u;
    o->inq = pa_asyncmsgq_new(0);
    o->outq = pa_asyncmsgq_new(0);
    o->inq_rtpoll_item_write = o->inq_rtpoll_item_read = NULL;
    o->outq_rtpoll_item_write = o->outq_rtpoll_item_read = NULL;
    o->sink = sink;
    o->sink_input = NULL;
    o->memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            MEMBLOCKQ_MAXLENGTH,
            pa_frame_size(&u->sink->sample_spec),
            1,
            0,
            0,
            NULL);
    pa_atomic_store(&o->max_request, 0);
    PA_LLIST_INIT(struct output, o);

    pa_assert_se(pa_idxset_put(u->outputs, o, NULL) == 0);

    state = pa_sink_get_state(u->sink);

    if (state != PA_SINK_INIT)
        pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_ADD_OUTPUT, o, 0, NULL);
    else {
        /* If the sink is not yet started, we need to do the activation ourselves */
        PA_LLIST_PREPEND(struct output, u->thread_info.active_outputs, o);

        o->outq_rtpoll_item_read = pa_rtpoll_item_new_asyncmsgq_read(
                u->rtpoll,
                PA_RTPOLL_EARLY-1,  /* This item is very important */
                o->outq);
        o->inq_rtpoll_item_write = pa_rtpoll_item_new_asyncmsgq_write(
                u->rtpoll,
                PA_RTPOLL_EARLY,
                o->inq);
    }

    if (PA_SINK_IS_OPENED(state) || state == PA_SINK_INIT) {
        pa_sink_suspend(sink, FALSE);

        if (PA_SINK_IS_OPENED(pa_sink_get_state(sink)))
            if (output_create_sink_input(o) < 0)
                goto fail;
    }

    update_description(u);

    return o;

fail:

    if (o) {
        pa_idxset_remove_by_data(u->outputs, o, NULL);

        if (o->sink_input) {
            pa_sink_input_unlink(o->sink_input);
            pa_sink_input_unref(o->sink_input);
        }

        if (o->memblockq)
            pa_memblockq_free(o->memblockq);

        if (o->inq)
            pa_asyncmsgq_unref(o->inq);

        if (o->outq)
            pa_asyncmsgq_unref(o->outq);

        pa_xfree(o);
    }

    return NULL;
}

static pa_bool_t is_suitable_sink(struct userdata *u, pa_sink *s) {
    const char *t;

    pa_sink_assert_ref(s);

    if (!(s->flags & PA_SINK_HARDWARE))
        return FALSE;

    if (s == u->sink)
        return FALSE;

    if ((t = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_CLASS)))
        if (strcmp(t, "sound"))
            return FALSE;

    return TRUE;
}

static pa_hook_result_t sink_put_hook_cb(pa_core *c, pa_sink *s, struct userdata* u) {
    struct output *o;

    pa_core_assert_ref(c);
    pa_sink_assert_ref(s);
    pa_assert(u);
    pa_assert(u->automatic);

    if (!is_suitable_sink(u, s))
        return PA_HOOK_OK;

    pa_log_info("Configuring new sink: %s", s->name);

    if (!(o = output_new(u, s))) {
        pa_log("Failed to create sink input on sink '%s'.", s->name);
        return PA_HOOK_OK;
    }

    if (o->sink_input)
        pa_sink_input_put(o->sink_input);

    return PA_HOOK_OK;
}

static struct output* find_output(struct userdata *u, pa_sink *s) {
    struct output *o;
    uint32_t idx;

    pa_assert(u);
    pa_assert(s);

    if (u->sink == s)
        return NULL;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        if (o->sink == s)
            return o;

    return NULL;
}

static pa_hook_result_t sink_unlink_hook_cb(pa_core *c, pa_sink *s, struct userdata* u) {
    struct output *o;

    pa_assert(c);
    pa_sink_assert_ref(s);
    pa_assert(u);

    if (!(o = find_output(u, s)))
        return PA_HOOK_OK;

    pa_log_info("Unconfiguring sink: %s", s->name);

    output_free(o);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_state_changed_hook_cb(pa_core *c, pa_sink *s, struct userdata* u) {
    struct output *o;
    pa_sink_state_t state;

    if (!(o = find_output(u, s)))
        return PA_HOOK_OK;

    state = pa_sink_get_state(s);

    if (PA_SINK_IS_OPENED(state) && PA_SINK_IS_OPENED(pa_sink_get_state(u->sink)) && !o->sink_input)
        enable_output(o);

    if (state == PA_SINK_SUSPENDED && o->sink_input)
        disable_output(o);

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    const char *slaves, *rm;
    int resample_method = PA_RESAMPLER_TRIVIAL;
    pa_sample_spec ss;
    pa_channel_map map;
    struct output *o;
    uint32_t idx;
    pa_sink_new_data data;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    if ((rm = pa_modargs_get_value(ma, "resample_method", NULL))) {
        if ((resample_method = pa_parse_resample_method(rm)) < 0) {
            pa_log("invalid resample method '%s'", rm);
            goto fail;
        }
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->sink = NULL;
    u->time_event = NULL;
    u->adjust_time = DEFAULT_ADJUST_TIME;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);
    u->thread = NULL;
    u->resample_method = resample_method;
    u->outputs = pa_idxset_new(NULL, NULL);
    memset(&u->adjust_timestamp, 0, sizeof(u->adjust_timestamp));
    u->sink_put_slot = u->sink_unlink_slot = u->sink_state_changed_slot = NULL;
    PA_LLIST_HEAD_INIT(struct output, u->thread_info.active_outputs);
    pa_atomic_store(&u->thread_info.running, FALSE);
    u->thread_info.in_null_mode = FALSE;
    u->thread_info.counter = 0;
    u->thread_info.smoother = pa_smoother_new(
            PA_USEC_PER_SEC,
            PA_USEC_PER_SEC*2,
            TRUE,
            TRUE,
            10,
            0,
            FALSE);

    if (pa_modargs_get_value_u32(ma, "adjust_time", &u->adjust_time) < 0) {
        pa_log("Failed to parse adjust_time value");
        goto fail;
    }

    slaves = pa_modargs_get_value(ma, "slaves", NULL);
    u->automatic = !slaves;

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if ((pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0)) {
        pa_log("Invalid sample specification.");
        goto fail;
    }

    pa_sink_new_data_init(&data);
    data.namereg_fail = FALSE;
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Simultaneous Output");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "filter");

    if (slaves)
        pa_proplist_sets(data.proplist, "combine.slaves", slaves);

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state = sink_set_state;
    u->sink->userdata = u;

    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);

    u->block_usec = BLOCK_USEC;
    pa_sink_set_max_request(u->sink, pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec));

    if (!u->automatic) {
        const char*split_state;
        char *n = NULL;
        pa_assert(slaves);

        /* The slaves have been specified manually */

        split_state = NULL;
        while ((n = pa_split(slaves, ",", &split_state))) {
            pa_sink *slave_sink;

            if (!(slave_sink = pa_namereg_get(m->core, n, PA_NAMEREG_SINK)) || slave_sink == u->sink) {
                pa_log("Invalid slave sink '%s'", n);
                pa_xfree(n);
                goto fail;
            }

            pa_xfree(n);

            if (!output_new(u, slave_sink)) {
                pa_log("Failed to create slave sink input on sink '%s'.", slave_sink->name);
                goto fail;
            }
        }

        if (pa_idxset_size(u->outputs) <= 1)
            pa_log_warn("No slave sinks specified.");

        u->sink_put_slot = NULL;

    } else {
        pa_sink *s;

        /* We're in automatic mode, we add every sink that matches our needs  */

        for (s = pa_idxset_first(m->core->sinks, &idx); s; s = pa_idxset_next(m->core->sinks, &idx)) {

            if (!is_suitable_sink(u, s))
                continue;

            if (!output_new(u, s)) {
                pa_log("Failed to create sink input on sink '%s'.", s->name);
                goto fail;
            }
        }

        u->sink_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_put_hook_cb, u);
    }

    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_EARLY, (pa_hook_cb_t) sink_unlink_hook_cb, u);
    u->sink_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_state_changed_hook_cb, u);

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    /* Activate the sink and the sink inputs */
    pa_sink_put(u->sink);

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        if (o->sink_input)
            pa_sink_input_put(o->sink_input);

    if (u->adjust_time > 0) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec += (time_t) u->adjust_time;
        u->time_event = m->core->mainloop->time_new(m->core->mainloop, &tv, time_callback, u);
    }

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

static void output_free(struct output *o) {
    pa_assert(o);

    disable_output(o);

    pa_assert_se(pa_idxset_remove_by_data(o->userdata->outputs, o, NULL));

    update_description(o->userdata);

    if (o->inq_rtpoll_item_read)
        pa_rtpoll_item_free(o->inq_rtpoll_item_read);
    if (o->inq_rtpoll_item_write)
        pa_rtpoll_item_free(o->inq_rtpoll_item_write);

    if (o->outq_rtpoll_item_read)
        pa_rtpoll_item_free(o->outq_rtpoll_item_read);
    if (o->outq_rtpoll_item_write)
        pa_rtpoll_item_free(o->outq_rtpoll_item_write);

    if (o->inq)
        pa_asyncmsgq_unref(o->inq);

    if (o->outq)
        pa_asyncmsgq_unref(o->outq);

    if (o->memblockq)
        pa_memblockq_free(o->memblockq);

    pa_xfree(o);
}

void pa__done(pa_module*m) {
    struct userdata *u;
    struct output *o;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_put_slot)
        pa_hook_slot_free(u->sink_put_slot);

    if (u->sink_unlink_slot)
        pa_hook_slot_free(u->sink_unlink_slot);

    if (u->sink_state_changed_slot)
        pa_hook_slot_free(u->sink_state_changed_slot);

    if (u->outputs) {
        while ((o = pa_idxset_first(u->outputs, NULL)))
            output_free(o);

        pa_idxset_free(u->outputs, NULL, NULL);
    }

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->time_event)
        u->core->mainloop->time_free(u->time_event);

    if (u->thread_info.smoother)
        pa_smoother_free(u->thread_info.smoother);

    pa_xfree(u);
}
