/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include "module-combine-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Combine multiple sinks to one")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "master=<master sink> "
        "slaves=<slave sinks> "
        "adjust_time=<seconds> "
        "resample_method=<method> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>")

#define DEFAULT_SINK_NAME "combined"
#define MEMBLOCKQ_MAXLENGTH (1024*170)

#define DEFAULT_ADJUST_TIME 10

static const char* const valid_modargs[] = {
    "sink_name",
    "master",
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
    pa_rtpoll_item *inq_rtpoll_item, *outq_rtpoll_item;

    pa_memblockq *memblockq;

    pa_usec_t total_latency;

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
    size_t block_size;

    pa_hook_slot *sink_new_slot, *sink_unlink_slot, *sink_state_changed_slot;

    pa_resample_method_t resample_method;

    struct timeval adjust_timestamp;

    struct output *master;
    pa_idxset* outputs; /* managed in main context */

    struct {
        PA_LLIST_HEAD(struct output, active_outputs); /* managed in IO thread context */
        pa_atomic_t running;  /* we cache that value here, so that every thread can query it cheaply */
        struct timeval timestamp;
        pa_bool_t in_null_mode;
    } thread_info;
};

enum {
    SINK_MESSAGE_ADD_OUTPUT = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_REMOVE_OUTPUT,
    SINK_MESSAGE_NEED
};

enum {
    SINK_INPUT_MESSAGE_POST = PA_SINK_INPUT_MESSAGE_MAX
};

static void output_free(struct output *o);
static int output_create_sink_input(struct output *o);
static void update_master(struct userdata *u, struct output *o);
static void pick_master(struct userdata *u, struct output *except);

static void adjust_rates(struct userdata *u) {
    struct output *o;
    pa_usec_t max_sink_latency = 0, min_total_latency = (pa_usec_t) -1, target_latency;
    uint32_t base_rate;
    uint32_t idx;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (pa_idxset_size(u->outputs) <= 0)
        return;

    if (!u->master)
        return;

    if (!PA_SINK_OPENED(pa_sink_get_state(u->sink)))
        return;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        pa_usec_t sink_latency;

        if (!o->sink_input || !PA_SINK_OPENED(pa_sink_get_state(o->sink)))
            continue;

        sink_latency = pa_sink_get_latency(o->sink);
        o->total_latency = sink_latency + pa_sink_input_get_latency(o->sink_input);

        if (sink_latency > max_sink_latency)
            max_sink_latency = sink_latency;

        if (min_total_latency == (pa_usec_t) -1 || o->total_latency < min_total_latency)
            min_total_latency = o->total_latency;
    }

    if (min_total_latency == (pa_usec_t) -1)
        return;

    target_latency = max_sink_latency > min_total_latency ? max_sink_latency : min_total_latency;

    pa_log_info("[%s] target latency is %0.0f usec.", u->sink->name, (float) target_latency);
    pa_log_info("[%s] master %s latency %0.0f usec.", u->sink->name, u->master->sink->name, (float) u->master->total_latency);

    base_rate = u->sink->sample_spec.rate;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        uint32_t r = base_rate;

        if (!o->sink_input || !PA_SINK_OPENED(pa_sink_get_state(o->sink)))
            continue;

        if (o->total_latency < target_latency)
            r -= (uint32_t) (((((double) target_latency - o->total_latency))/u->adjust_time)*r/PA_USEC_PER_SEC);
        else if (o->total_latency > target_latency)
            r += (uint32_t) (((((double) o->total_latency - target_latency))/u->adjust_time)*r/PA_USEC_PER_SEC);

        if (r < (uint32_t) (base_rate*0.9) || r > (uint32_t) (base_rate*1.1)) {
            pa_log_warn("[%s] sample rates too different, not adjusting (%u vs. %u).", o->sink_input->name, base_rate, r);
            pa_sink_input_set_rate(o->sink_input, base_rate);
        } else {
            pa_log_info("[%s] new rate is %u Hz; ratio is %0.3f; latency is %0.0f usec.", o->sink_input->name, r, (double) r / base_rate, (float) o->total_latency);
            pa_sink_input_set_rate(o->sink_input, r);
        }
    }
}

static void time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval n;

    pa_assert(u);
    pa_assert(a);
    pa_assert(u->time_event == e);

    adjust_rates(u);

    pa_gettimeofday(&n);
    n.tv_sec += u->adjust_time;
    u->sink->core->mainloop->time_restart(e, &n);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->high_priority)
        pa_make_realtime();

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    pa_rtclock_get(&u->thread_info.timestamp);
    u->thread_info.in_null_mode = FALSE;

    for (;;) {
        int ret;

        /* If no outputs are connected, render some data and drop it immediately. */
        if (u->sink->thread_info.state == PA_SINK_RUNNING && !u->thread_info.active_outputs) {
            struct timeval now;

            pa_rtclock_get(&now);

            if (!u->thread_info.in_null_mode || pa_timeval_cmp(&u->thread_info.timestamp, &now) <= 0) {
                pa_sink_skip(u->sink, u->block_size);

                if (!u->thread_info.in_null_mode)
                    u->thread_info.timestamp = now;

                pa_timeval_add(&u->thread_info.timestamp, pa_bytes_to_usec(u->block_size, &u->sink->sample_spec));
            }

            pa_rtpoll_set_timer_absolute(u->rtpoll, &u->thread_info.timestamp);
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
     * other output is waiting for us, hence it is safe to access its
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
        pa_asyncmsgq_send(o->outq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_NEED, o, length, NULL);
}

/* Called from I/O thread context */
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    /* If necessary, get some new data */
    request_memblock(o, length);

    return pa_memblockq_peek(o->memblockq, chunk);
}

/* Called from I/O thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert(length > 0);
    pa_assert_se(o = i->userdata);

    pa_memblockq_drop(o->memblockq, length);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    /* Set up the queue from the sink thread to us */
    pa_assert(!o->inq_rtpoll_item);
    o->inq_rtpoll_item = pa_rtpoll_item_new_asyncmsgq(
            i->sink->rtpoll,
            PA_RTPOLL_LATE,  /* This one is not that important, since we check for data in _peek() anyway. */
            o->inq);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert_se(o = i->userdata);

    /* Shut down the queue from the sink thread to us */
    pa_assert(o->inq_rtpoll_item);
    pa_rtpoll_item_free(o->inq_rtpoll_item);
    o->inq_rtpoll_item = NULL;
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert(o = i->userdata);

    pa_module_unload_request(o->userdata->module);
    output_free(o);
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

            if (PA_SINK_OPENED(o->sink_input->sink->thread_info.state))
                pa_memblockq_push_align(o->memblockq, chunk);
            else
                pa_memblockq_flush(o->memblockq);

            break;
    }

    return pa_sink_input_process_msg(obj, code, data, offset, chunk);
}

/* Called from main context */
static void disable_output(struct output *o) {
    pa_assert(o);

    if (!o->sink_input)
        return;

    pa_asyncmsgq_send(o->userdata->sink->asyncmsgq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_REMOVE_OUTPUT, o, 0, NULL);
    pa_sink_input_unlink(o->sink_input);
    pa_sink_input_unref(o->sink_input);
    o->sink_input = NULL;

}

/* Called from main context */
static void enable_output(struct output *o) {
    pa_assert(o);

    if (o->sink_input)
        return;

    if (output_create_sink_input(o) >= 0) {

        pa_memblockq_flush(o->memblockq);

        pa_sink_input_put(o->sink_input);

        if (o->userdata->sink && PA_SINK_LINKED(pa_sink_get_state(o->userdata->sink)))
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

    pick_master(u, NULL);

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

        if (PA_SINK_OPENED(pa_sink_get_state(o->sink)))
            enable_output(o);
    }

    pick_master(u, NULL);

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
            pa_assert(PA_SINK_OPENED(pa_sink_get_state(u->sink)));

            suspend(u);
            break;

        case PA_SINK_IDLE:
        case PA_SINK_RUNNING:

            if (pa_sink_get_state(u->sink) == PA_SINK_SUSPENDED)
                unsuspend(u);

            break;

        case PA_SINK_UNLINKED:
        case PA_SINK_INIT:
            ;
    }

    return 0;
}

/* Called from thread context of the master */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_SET_STATE:
            pa_atomic_store(&u->thread_info.running, PA_PTR_TO_UINT(data) == PA_SINK_RUNNING);
            break;

        case PA_SINK_MESSAGE_GET_LATENCY:

            /* This code will only be called when running in NULL
             * mode, i.e. when no output is attached. See
             * sink_get_latency_cb() below */

            if (u->thread_info.in_null_mode) {
                struct timeval now;

                if (pa_timeval_cmp(&u->thread_info.timestamp, pa_rtclock_get(&now)) > 0) {
                    *((pa_usec_t*) data) = pa_timeval_diff(&u->thread_info.timestamp, &now);
                    break;
                }
            }

            *((pa_usec_t*) data) = 0;

            break;

        case SINK_MESSAGE_ADD_OUTPUT: {
            struct output *op = data;

            PA_LLIST_PREPEND(struct output, u->thread_info.active_outputs, op);

            pa_assert(!op->outq_rtpoll_item);

            /* Create pa_asyncmsgq to the sink thread */

            op->outq_rtpoll_item = pa_rtpoll_item_new_asyncmsgq(
                    u->rtpoll,
                    PA_RTPOLL_EARLY-1,  /* This item is very important */
                    op->outq);

            return 0;
        }

        case SINK_MESSAGE_REMOVE_OUTPUT: {
            struct output *op = data;

            PA_LLIST_REMOVE(struct output, u->thread_info.active_outputs, op);

            /* Remove the q that leads from this output to the sink thread */

            pa_assert(op->outq_rtpoll_item);
            pa_rtpoll_item_free(op->outq_rtpoll_item);
            op->outq_rtpoll_item = NULL;

            return 0;
        }

        case SINK_MESSAGE_NEED:
            render_memblock(u, data, (size_t) offset);
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (u->master) {
        /* If we have a master sink, we just return the latency of it
         * and add our own buffering on top */

        if (!u->master->sink_input)
            return 0;

        return
            pa_sink_input_get_latency(u->master->sink_input) +
            pa_sink_get_latency(u->master->sink);

    } else {
        pa_usec_t usec = 0;

        /* We have no master, hence let's ask our own thread which
         * implements the NULL sink */

        if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
            return 0;

        return usec;
    }
}

static void update_description(struct userdata *u) {
    int first = 1;
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
            e = pa_sprintf_malloc("%s %s", t, o->sink->description);
            first = 0;
        } else
            e = pa_sprintf_malloc("%s, %s", t, o->sink->description);

        pa_xfree(t);
        t = e;
    }

    pa_sink_set_description(u->sink, t);
    pa_xfree(t);
}

static void update_master(struct userdata *u, struct output *o) {
    pa_assert(u);

    if (u->master == o)
        return;

    if ((u->master = o))
        pa_log_info("Master sink is now '%s'", o->sink_input->sink->name);
    else
        pa_log_info("No master selected, lacking suitable outputs.");
}

static void pick_master(struct userdata *u, struct output *except) {
    struct output *o;
    uint32_t idx;
    pa_assert(u);

    if (u->master &&
        u->master != except &&
        u->master->sink_input &&
        PA_SINK_OPENED(pa_sink_get_state(u->master->sink))) {
        update_master(u, u->master);
        return;
    }

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        if (o != except &&
            o->sink_input &&
            PA_SINK_OPENED(pa_sink_get_state(o->sink))) {
            update_master(u, o);
            return;
        }

    update_master(u, NULL);
}

static int output_create_sink_input(struct output *o) {
    pa_sink_input_new_data data;
    char *t;

    pa_assert(o);

    if (o->sink_input)
        return 0;

    t = pa_sprintf_malloc("Simultaneous output on %s", o->sink->description);

    pa_sink_input_new_data_init(&data);
    data.sink = o->sink;
    data.driver = __FILE__;
    data.name = t;
    pa_sink_input_new_data_set_sample_spec(&data, &o->userdata->sink->sample_spec);
    pa_sink_input_new_data_set_channel_map(&data, &o->userdata->sink->channel_map);
    data.module = o->userdata->module;
    data.resample_method = o->userdata->resample_method;

    o->sink_input = pa_sink_input_new(o->userdata->core, &data, PA_SINK_INPUT_VARIABLE_RATE|PA_SINK_INPUT_DONT_MOVE);

    pa_xfree(t);

    if (!o->sink_input)
        return -1;

    o->sink_input->parent.process_msg = sink_input_process_msg;
    o->sink_input->peek = sink_input_peek_cb;
    o->sink_input->drop = sink_input_drop_cb;
    o->sink_input->attach = sink_input_attach_cb;
    o->sink_input->detach = sink_input_detach_cb;
    o->sink_input->kill = sink_input_kill_cb;
    o->sink_input->userdata = o;


    return 0;
}

static struct output *output_new(struct userdata *u, pa_sink *sink) {
    struct output *o;

    pa_assert(u);
    pa_assert(sink);
    pa_assert(u->sink);

    o = pa_xnew(struct output, 1);
    o->userdata = u;
    o->inq = pa_asyncmsgq_new(0);
    o->outq = pa_asyncmsgq_new(0);
    o->inq_rtpoll_item = NULL;
    o->outq_rtpoll_item = NULL;
    o->sink = sink;
    o->sink_input = NULL;
    o->memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            MEMBLOCKQ_MAXLENGTH,
            pa_frame_size(&u->sink->sample_spec),
            1,
            0,
            NULL);

    pa_assert_se(pa_idxset_put(u->outputs, o, NULL) == 0);

    if (u->sink && PA_SINK_LINKED(pa_sink_get_state(u->sink)))
        pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_ADD_OUTPUT, o, 0, NULL);
    else {
        /* If the sink is not yet started, we need to do the activation ourselves */
        PA_LLIST_PREPEND(struct output, u->thread_info.active_outputs, o);

        o->outq_rtpoll_item = pa_rtpoll_item_new_asyncmsgq(
                u->rtpoll,
                PA_RTPOLL_EARLY-1,  /* This item is very important */
                o->outq);
    }

    if (PA_SINK_OPENED(pa_sink_get_state(u->sink)) || pa_sink_get_state(u->sink) == PA_SINK_INIT) {
        pa_sink_suspend(sink, FALSE);

        if (PA_SINK_OPENED(pa_sink_get_state(sink)))
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

static pa_hook_result_t sink_new_hook_cb(pa_core *c, pa_sink *s, struct userdata* u) {
    struct output *o;

    pa_core_assert_ref(c);
    pa_sink_assert_ref(s);
    pa_assert(u);
    pa_assert(u->automatic);

    if (!(s->flags & PA_SINK_HARDWARE) || s == u->sink)
        return PA_HOOK_OK;

    pa_log_info("Configuring new sink: %s", s->name);

    if (!(o = output_new(u, s))) {
        pa_log("Failed to create sink input on sink '%s'.", s->name);
        return PA_HOOK_OK;
    }

    if (o->sink_input)
        pa_sink_input_put(o->sink_input);

    pick_master(u, NULL);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_unlink_hook_cb(pa_core *c, pa_sink *s, struct userdata* u) {
    struct output *o;
    uint32_t idx;

    pa_assert(c);
    pa_sink_assert_ref(s);
    pa_assert(u);

    if (s == u->sink)
        return PA_HOOK_OK;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        if (o->sink == s)
            break;

    if (!o)
        return PA_HOOK_OK;

    pa_log_info("Unconfiguring sink: %s", s->name);

    output_free(o);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_state_changed_hook_cb(pa_core *c, pa_sink *s, struct userdata* u) {
    struct output *o;
    uint32_t idx;
    pa_sink_state_t state;

    if (s == u->sink)
        return PA_HOOK_OK;

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        if (o->sink == s)
            break;

    if (!o)
        return PA_HOOK_OK;

    state = pa_sink_get_state(s);

    if (PA_SINK_OPENED(state) && PA_SINK_OPENED(pa_sink_get_state(u->sink)) && !o->sink_input) {
        enable_output(o);
        pick_master(u, NULL);
    }

    if (state == PA_SINK_SUSPENDED && o->sink_input) {
        disable_output(o);
        pick_master(u, o);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    const char *master_name, *slaves, *rm;
    pa_sink *master_sink = NULL;
    int resample_method = PA_RESAMPLER_TRIVIAL;
    pa_sample_spec ss;
    pa_channel_map map;
    struct output *o;
    uint32_t idx;

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

    u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->sink = NULL;
    u->master = NULL;
    u->time_event = NULL;
    u->adjust_time = DEFAULT_ADJUST_TIME;
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);
    u->rtpoll = pa_rtpoll_new();
    u->thread = NULL;
    u->resample_method = resample_method;
    u->outputs = pa_idxset_new(NULL, NULL);
    memset(&u->adjust_timestamp, 0, sizeof(u->adjust_timestamp));
    u->sink_new_slot = u->sink_unlink_slot = u->sink_state_changed_slot = NULL;
    PA_LLIST_HEAD_INIT(struct output, u->thread_info.active_outputs);
    pa_atomic_store(&u->thread_info.running, FALSE);
    u->thread_info.in_null_mode = FALSE;
    pa_rtpoll_item_new_asyncmsgq(u->rtpoll, PA_RTPOLL_EARLY, u->thread_mq.inq);

    if (pa_modargs_get_value_u32(ma, "adjust_time", &u->adjust_time) < 0) {
        pa_log("Failed to parse adjust_time value");
        goto fail;
    }

    master_name = pa_modargs_get_value(ma, "master", NULL);
    slaves = pa_modargs_get_value(ma, "slaves", NULL);
    if (!master_name != !slaves) {
        pa_log("No master or slave sinks specified");
        goto fail;
    }

    if (master_name) {
        if (!(master_sink = pa_namereg_get(m->core, master_name, PA_NAMEREG_SINK, 1))) {
            pa_log("Invalid master sink '%s'", master_name);
            goto fail;
        }

        ss = master_sink->sample_spec;
        u->automatic = FALSE;
    } else {
        master_sink = NULL;
        ss = m->core->default_sample_spec;
        u->automatic = TRUE;
    }

    if ((pa_modargs_get_sample_spec(ma, &ss) < 0)) {
        pa_log("Invalid sample specification.");
        goto fail;
    }

    if (master_sink && ss.channels == master_sink->sample_spec.channels)
        map = master_sink->channel_map;
    else
        pa_channel_map_init_auto(&map, ss.channels, PA_CHANNEL_MAP_DEFAULT);

    if ((pa_modargs_get_channel_map(ma, NULL, &map) < 0)) {
        pa_log("Invalid channel map.");
        goto fail;
    }

    if (ss.channels != map.channels) {
        pa_log("Channel map and sample specification don't match.");
        goto fail;
    }

    if (!(u->sink = pa_sink_new(m->core, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log("Failed to create sink");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->get_latency = sink_get_latency_cb;
    u->sink->set_state = sink_set_state;
    u->sink->userdata = u;

    u->sink->flags = PA_SINK_LATENCY;
    pa_sink_set_module(u->sink, m);
    pa_sink_set_description(u->sink, "Simultaneous output");
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);

    u->block_size = pa_bytes_per_second(&ss) / 20; /* 50 ms */
    if (u->block_size <= 0)
        u->block_size = pa_frame_size(&ss);

    if (!u->automatic) {
        const char*split_state;
        char *n = NULL;
        pa_assert(slaves);

        /* The master and slaves have been specified manually */

        if (!(u->master = output_new(u, master_sink))) {
            pa_log("Failed to create master sink input on sink '%s'.", master_sink->name);
            goto fail;
        }

        split_state = NULL;
        while ((n = pa_split(slaves, ",", &split_state))) {
            pa_sink *slave_sink;

            if (!(slave_sink = pa_namereg_get(m->core, n, PA_NAMEREG_SINK, 1)) || slave_sink == u->sink) {
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

        u->sink_new_slot = NULL;

    } else {
        pa_sink *s;

        /* We're in automatic mode, we elect one hw sink to the master
         * and attach all other hw sinks as slaves to it */

        for (s = pa_idxset_first(m->core->sinks, &idx); s; s = pa_idxset_next(m->core->sinks, &idx)) {

            if (!(s->flags & PA_SINK_HARDWARE) || s == u->sink)
                continue;

            if (!output_new(u, s)) {
                pa_log("Failed to create sink input on sink '%s'.", s->name);
                goto fail;
            }
        }

        u->sink_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_NEW_POST], (pa_hook_cb_t) sink_new_hook_cb, u);
    }

    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], (pa_hook_cb_t) sink_unlink_hook_cb, u);
    u->sink_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], (pa_hook_cb_t) sink_state_changed_hook_cb, u);

    pick_master(u, NULL);

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
        tv.tv_sec += u->adjust_time;
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

    pick_master(o->userdata, o);

    disable_output(o);

    pa_assert_se(pa_idxset_remove_by_data(o->userdata->outputs, o, NULL));

    update_description(o->userdata);

    if (o->inq_rtpoll_item)
        pa_rtpoll_item_free(o->inq_rtpoll_item);

    if (o->outq_rtpoll_item)
        pa_rtpoll_item_free(o->outq_rtpoll_item);

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

    if (u->sink_new_slot)
        pa_hook_slot_free(u->sink_new_slot);

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

    pa_xfree(u);
}
