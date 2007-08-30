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

    pa_asyncmsgq *asyncmsgq;
    pa_rtpoll_item *rtpoll_item;
    
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

    pa_mutex *mutex;
    
    struct output *master;

    pa_time_event *time_event; 
    uint32_t adjust_time; 

    int automatic;
    size_t block_size;

    struct timespec timestamp;

    pa_hook_slot *sink_new_slot, *sink_unlink_slot, *sink_state_changed_slot;

    pa_resample_method_t resample_method;

    struct timespec adjust_timestamp;
    
    pa_idxset* outputs; /* managed in main context */

    struct {
        PA_LLIST_HEAD(struct output, outputs); /* managed in IO thread context */
        struct output *master;
    } thread_info;
};

enum {
    SINK_MESSAGE_DETACH = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_ATTACH,
    SINK_MESSAGE_ADD_OUTPUT,
    SINK_MESSAGE_REMOVE_OUTPUT
};

static void output_free(struct output *o);
static int output_create_sink_input(struct userdata *u, struct output *o);
static int update_master(struct userdata *u, struct output *o);
static int pick_master(struct userdata *u);

static void adjust_rates(struct userdata *u) {
    struct output *o;
    pa_usec_t max_sink_latency = 0, min_total_latency = (pa_usec_t) -1, target_latency;
    uint32_t base_rate;
    uint32_t idx;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (pa_idxset_size(u->outputs) <= 0)
        return;

    if (!PA_SINK_OPENED(pa_sink_get_state(u->sink)))
        return;
    
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        uint32_t sink_latency;

        if (!o->sink_input || !PA_SINK_OPENED(pa_sink_get_state(o->sink)))
            continue;

        sink_latency = o->sink_input->sink ? pa_sink_get_latency(o->sink_input->sink) : 0;
        o->total_latency = sink_latency + pa_sink_input_get_latency(o->sink_input);
        
        if (sink_latency > max_sink_latency)
            max_sink_latency = sink_latency;
        
        if (o->total_latency < min_total_latency)
            min_total_latency = o->total_latency;
    }

    if (min_total_latency == (pa_usec_t) -1)
        return;
        
    target_latency = max_sink_latency > min_total_latency ? max_sink_latency : min_total_latency;
        
    pa_log_info("[%s] target latency is %0.0f usec.", u->sink->name, (float) target_latency);
    pa_log_info("[%s] master is %s", u->sink->name, u->master->sink->description);
        
    base_rate = u->sink->sample_spec.rate;
        
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        uint32_t r = base_rate;

        if (!o->sink_input || !PA_SINK_OPENED(pa_sink_get_state(o->sink)))
            continue;
        
        if (o->total_latency < target_latency)
            r -= (uint32_t) (((((double) target_latency - o->total_latency))/u->adjust_time)*r/ 1000000);
        else if (o->total_latency > target_latency)
            r += (uint32_t) (((((double) o->total_latency - target_latency))/u->adjust_time)*r/ 1000000);
        
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

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    pa_rtclock_get(&u->timestamp);

    /* This is only run when were are in NULL mode, to make sure that
     * playback doesn't stop. In all other cases we hook our stuff
     * into the master sink. */
    
    for (;;) {
        int ret;

        /* Render some data and drop it immediately */
        if (u->sink->thread_info.state == PA_SINK_RUNNING) {
            struct timespec now;
            
            pa_rtclock_get(&now);

            if (pa_timespec_cmp(&u->timestamp, &now) <= 0) {
                pa_sink_skip(u->sink, u->block_size);
                pa_timespec_add(&u->timestamp, pa_bytes_to_usec(u->block_size, &u->sink->sample_spec));
            }

            pa_rtpoll_set_timer_absolute(u->rtpoll, &u->timestamp);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Now give the sink inputs some to time to process their data */
        if ((ret = pa_sink_process_inputs(u->sink)) < 0)
            goto fail;
        if (ret > 0)
            continue;

        /* Check whether there is a message for us to process */
        if ((ret = pa_thread_mq_process(&u->thread_mq) < 0))
            goto finish;
        if (ret > 0)
            continue;

        /* Hmm, nothing to do. Let's sleep */
        if (pa_rtpoll_run(u->rtpoll) < 0) {
            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

static void request_memblock(struct output *o) {
    pa_memchunk chunk;
    
    pa_assert(o);
    pa_sink_input_assert_ref(o->sink_input);
    pa_sink_assert_ref(o->userdata->sink);
    
    /* If another thread already prepared some data we received
     * the data over the asyncmsgq, hence let's first process
     * it. */
    while (pa_asyncmsgq_get(o->asyncmsgq, NULL, NULL, NULL, NULL, &chunk, 0) == 0) {
        pa_memblockq_push_align(o->memblockq, &chunk);
        pa_asyncmsgq_done(o->asyncmsgq, 0);
    }
    
    /* Check whether we're now readable */
    if (pa_memblockq_is_readable(o->memblockq))
        return;
    
    /* OK, we need to prepare new data */
    pa_mutex_lock(o->userdata->mutex);

    if (PA_SINK_OPENED(o->userdata->sink->thread_info.state)) {
    
        /* Maybe there's some data now? */
        while (pa_asyncmsgq_get(o->asyncmsgq, NULL, NULL, NULL, NULL, &chunk, 0) == 0) {
            pa_memblockq_push_align(o->memblockq, &chunk);
            pa_asyncmsgq_done(o->asyncmsgq, 0);
        }
        
        /* Ok, now let's prepare some data if we really have to */
        while (!pa_memblockq_is_readable(o->memblockq)) {
            struct output *j;
            
            /* Do it! */
            pa_sink_render(o->userdata->sink, o->userdata->block_size, &chunk);
            
            /* OK, let's send this data to the other threads */
            for (j = o->userdata->thread_info.outputs; j; j = j->next)
                if (j != o && j->sink_input)
                    pa_asyncmsgq_post(j->asyncmsgq, NULL, 0, NULL, 0, &chunk, NULL);
            
            /* And push it into our own queue */
            pa_memblockq_push_align(o->memblockq, &chunk);
            pa_memblock_unref(chunk.memblock);
        }
    }
    
    pa_mutex_unlock(o->userdata->mutex);
}

/* Called from I/O trhead context */
static int sink_input_peek_cb(pa_sink_input *i, pa_memchunk *chunk) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    o = i->userdata;
    pa_assert(o);

    /* If necessary, get some new data */
    request_memblock(o);

    return  pa_memblockq_peek(o->memblockq, chunk);
}

/* Called from I/O thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    pa_assert(length > 0);
    o = i->userdata;
    pa_assert(o);

    pa_memblockq_drop(o->memblockq, length);
}

/* Called from I/O thread context */
static int sink_input_process_cb(pa_sink_input *i) {
    struct output *o;
    pa_memchunk chunk;
    int r = 0;
    
    pa_sink_input_assert_ref(i);
    o = i->userdata;
    pa_assert(o);

    /* Move all data in the asyncmsgq into our memblockq */
    
    while (pa_asyncmsgq_get(o->asyncmsgq, NULL, NULL, NULL, NULL, &chunk, 0) == 0) {
        if (PA_SINK_OPENED(i->sink->thread_info.state))
            pa_memblockq_push_align(o->memblockq, &chunk);
        pa_asyncmsgq_done(o->asyncmsgq, 0);
    }

    /* If the sink is suspended, flush our queue */
    if (!PA_SINK_OPENED(i->sink->thread_info.state))
        pa_memblockq_flush(o->memblockq);

    if (o == o->userdata->thread_info.master) {
        pa_mutex_lock(o->userdata->mutex);
        r = pa_sink_process_inputs(o->userdata->sink);
        pa_mutex_unlock(o->userdata->mutex);
    }
    
    return r;
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    o = i->userdata;
    pa_assert(o);

    pa_assert(!o->rtpoll_item);
    o->rtpoll_item = pa_rtpoll_item_new_asyncmsgq(i->sink->rtpoll, o->asyncmsgq);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    o = i->userdata;
    pa_assert(o);

    pa_assert(o->rtpoll_item);
    pa_rtpoll_item_free(o->rtpoll_item);
    o->rtpoll_item = NULL;
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct output *o;

    pa_sink_input_assert_ref(i);
    o = i->userdata;
    pa_assert(o);

    pa_sink_input_unlink(o->sink_input);
    pa_sink_input_unref(o->sink_input);
    o->sink_input = NULL;
    
    pa_module_unload_request(o->userdata->module);
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

    }
    
    return pa_sink_input_process_msg(obj, code, data, offset, chunk);
}

static int suspend(struct userdata *u) {
    struct output *o;
    uint32_t idx;
    
    pa_assert(u);

    /* Let's suspend by unlinking all streams */
    
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
        pa_sink_input_unlink(o->sink_input);
        pa_sink_input_unref(o->sink_input);
        o->sink_input = NULL;
    }

    if (pick_master(u) < 0)
        pa_module_unload_request(u->module);

    pa_log_info("Device suspended...");
    
    return 0;
}

static int unsuspend(struct userdata *u) {
    struct output *o;
    uint32_t idx;

    pa_assert(u);
    
    /* Let's resume */
    
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx)) {
            
        if (output_create_sink_input(u, o) < 0)
            output_free(o);
        else
            pa_sink_input_put(o->sink_input);
    }

    if (pick_master(u) < 0)
        pa_module_unload_request(u->module);
    
    pa_log_info("Resumed successfully...");
    return 0;
}

static int sink_set_state(pa_sink *sink, pa_sink_state_t state) {
    struct userdata *u;
    
    pa_sink_assert_ref(sink);
    u = sink->userdata;
    pa_assert(u);

    /* Please note that in contrast to the ALSA modules we call
     * suspend/unsuspend from main context here! */
    
    switch (state) {
        case PA_SINK_SUSPENDED:
            pa_assert(PA_SINK_OPENED(pa_sink_get_state(u->sink)));
            
            if (suspend(u) < 0)
                return -1;

            break;

        case PA_SINK_IDLE:
        case PA_SINK_RUNNING:

            if (pa_sink_get_state(u->sink) == PA_SINK_SUSPENDED) {
                if (unsuspend(u) < 0)
                    return -1;
            }
                    
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

            if ((pa_sink_state_t) PA_PTR_TO_UINT(data) == PA_SINK_RUNNING) {
                /* Only useful when running in NULL mode, i.e. when no
                 * master sink is attached */       
                pa_rtclock_get(&u->timestamp);
            }
            
            break;
            
        case PA_SINK_MESSAGE_GET_LATENCY: {
            struct timespec now;

            /* This code will only be called when running in NULL
             * mode, i.e. when no master sink is attached. See
             * sink_get_latency_cb() below */
            pa_rtclock_get(&now);
            
            if (pa_timespec_cmp(&u->timestamp, &now) > 0)
                *((pa_usec_t*) data) = 0;
            else
                *((pa_usec_t*) data) = pa_timespec_diff(&u->timestamp, &now);
            break;
        }

        case SINK_MESSAGE_DETACH: {
            pa_sink_input *i;
            void *state = NULL;

            /* We're detaching all our input streams artificially, so
             * that we can driver our sink from a different sink */

            while ((i = pa_hashmap_iterate(u->sink->thread_info.inputs, &state, NULL)))
                if (i->detach)
                    i->detach(i);

            u->thread_info.master = NULL;
            
            break;
        }

        case SINK_MESSAGE_ATTACH: {
            pa_sink_input *i;
            void *state = NULL;

            /* We're attached all our input streams artificially again */

            while ((i = pa_hashmap_iterate(u->sink->thread_info.inputs, &state, NULL)))
                if (i->attach)
                    i->attach(i);

            u->thread_info.master = data;
            
            break;
        }

        case SINK_MESSAGE_ADD_OUTPUT:
            PA_LLIST_PREPEND(struct output, u->thread_info.outputs, (struct output*) data);
            break;

        case SINK_MESSAGE_REMOVE_OUTPUT:
            PA_LLIST_REMOVE(struct output, u->thread_info.outputs, (struct output*) data);
            break;
    }
    
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    u = s->userdata;
    pa_assert(u);

    if (u->master) {
        /* If we have a master sink, we just return the latency of it
         * and add our own buffering on top */

        if (!u->master->sink_input)
            return 0;
        
        return
            pa_sink_input_get_latency(u->master->sink_input) +
            pa_sink_get_latency(u->master->sink_input->sink);
        
    } else {
        pa_usec_t usec;

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

static int update_master(struct userdata *u, struct output *o) {
    pa_assert(u);

    /* Make sure everything is detached from the old thread before we move our stuff to a new thread */
    if (u->sink && PA_SINK_LINKED(pa_sink_get_state(u->sink)))
        pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_DETACH, NULL, 0, NULL);
    
    if (o) {
        /* If we have a master sink we run our own sink in its thread */

        pa_assert(o->sink_input);
        pa_assert(PA_SINK_OPENED(pa_sink_get_state(o->sink)));
        
        if (u->thread) {
            /* If we previously were in NULL mode, let's kill the thread */
            pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
            pa_thread_free(u->thread);
            u->thread = NULL;

            pa_assert(u->rtpoll);
            pa_rtpoll_free(u->rtpoll);
            u->rtpoll = NULL;
        }

        pa_sink_set_asyncmsgq(u->sink, o->sink->asyncmsgq);
        pa_sink_set_rtpoll(u->sink, o->sink->rtpoll);
        u->master = o;

        pa_log_info("Master sink is now '%s'", o->sink_input->sink->name);

    } else {

        /* We have no master sink, let's create our own thread */
        
        pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
        u->master = NULL;

        if (!u->thread) {
            pa_assert(!u->rtpoll);
            
            u->rtpoll = pa_rtpoll_new();
            pa_rtpoll_item_new_asyncmsgq(u->rtpoll, u->thread_mq.inq);

            pa_sink_set_rtpoll(u->sink, u->rtpoll);
            
            if (!(u->thread = pa_thread_new(thread_func, u))) {
                pa_log("Failed to create thread.");
                return -1;
            }
        }
        
        pa_log_info("No suitable master sink found, going to NULL mode\n");
    }

    /* Now attach everything again */
    if (u->sink && PA_SINK_LINKED(pa_sink_get_state(u->sink)))
        pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_ATTACH, u->master, 0, NULL);

    return 0;
}

static int pick_master(struct userdata *u) {
    struct output *o;
    uint32_t idx;
    pa_assert(u);

    if (u->master && u->master->sink_input && PA_SINK_OPENED(pa_sink_get_state(u->master->sink)))
        return update_master(u, u->master);

    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
        if (o->sink_input && PA_SINK_OPENED(pa_sink_get_state(o->sink)))
            return update_master(u, o);

    return update_master(u, NULL);
}

static int output_create_sink_input(struct userdata *u, struct output *o) {
    pa_sink_input_new_data data;
    char *t;
    
    pa_assert(u);
    pa_assert(!o->sink_input);

    t = pa_sprintf_malloc("Simultaneous output on %s", o->sink->description);
    
    pa_sink_input_new_data_init(&data);
    data.sink = o->sink;
    data.driver = __FILE__;
    data.name = t;
    pa_sink_input_new_data_set_sample_spec(&data, &u->sink->sample_spec);
    pa_sink_input_new_data_set_channel_map(&data, &u->sink->channel_map);
    data.module = u->module;

    o->sink_input = pa_sink_input_new(u->core, &data, PA_SINK_INPUT_VARIABLE_RATE|PA_SINK_INPUT_DONT_MOVE);

    pa_xfree(t);

    if (!o->sink_input)
        return -1;
    
    o->sink_input->parent.process_msg = sink_input_process_msg;
    o->sink_input->peek = sink_input_peek_cb;
    o->sink_input->drop = sink_input_drop_cb;
    o->sink_input->process = sink_input_process_cb;
    o->sink_input->attach = sink_input_attach_cb;
    o->sink_input->detach = sink_input_detach_cb;
    o->sink_input->kill = sink_input_kill_cb;
    o->sink_input->userdata = o;
    
    return 0;
}

static struct output *output_new(struct userdata *u, pa_sink *sink, int resample_method) {
    struct output *o;

    pa_assert(u);
    pa_assert(sink);
    pa_assert(u->sink);

    o = pa_xnew(struct output, 1);
    o->userdata = u;
    o->asyncmsgq = pa_asyncmsgq_new(0);
    o->rtpoll_item = NULL;
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


    update_description(u);

    pa_assert_se(pa_idxset_put(u->outputs, o, NULL) == 0);
    if (u->sink && PA_SINK_LINKED(pa_sink_get_state(u->sink)))
        pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_ADD_OUTPUT, o, 0, NULL);
    else
        PA_LLIST_PREPEND(struct output, u->thread_info.outputs, o);

    if (PA_SINK_OPENED(pa_sink_get_state(sink)))
        if (output_create_sink_input(u, o) < 0)
            goto fail;

    return o;

fail:

    if (o) {
        if (o->sink_input) {
            pa_sink_input_unlink(o->sink_input);
            pa_sink_input_unref(o->sink_input);
        }

        if (o->memblockq)
            pa_memblockq_free(o->memblockq);

        if (o->asyncmsgq)
            pa_asyncmsgq_unref(o->asyncmsgq);

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
    
    if (!(o = output_new(u, s, u->resample_method))) {
        pa_log("Failed to create sink input on sink '%s'.", s->name);
        return PA_HOOK_OK;
    }

    if (pick_master(u) < 0)
        pa_module_unload_request(u->module);
    
    if (o->sink_input)
        pa_sink_input_put(o->sink_input);
        
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

    if (pick_master(u) < 0)
        pa_module_unload_request(u->module);
    
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
    
    if (PA_SINK_OPENED(state) && !o->sink_input) {
        output_create_sink_input(u, o);

        if (pick_master(u) < 0)
            pa_module_unload_request(u->module);

        if (o->sink_input)
            pa_sink_input_put(o->sink_input);
    }
        
    if (state == PA_SINK_SUSPENDED && o->sink_input) {
        pa_sink_input_unlink(o->sink_input);
        pa_sink_input_unref(o->sink_input);
        o->sink_input = NULL;

        pa_memblockq_flush(o->memblockq);

        if (pick_master(u) < 0)
            pa_module_unload_request(u->module);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    const char *master_name, *slaves, *rm;
    pa_sink *master_sink = NULL;
    int resample_method = -1;
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
    u->thread_info.master = u->master = NULL;
    u->time_event = NULL; 
    u->adjust_time = DEFAULT_ADJUST_TIME; 
    u->mutex = pa_mutex_new(0);
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);
    u->rtpoll = NULL;
    u->thread = NULL;
    PA_LLIST_HEAD_INIT(struct output, u->thread_info.outputs);
    u->resample_method = resample_method;
    u->outputs = pa_idxset_new(NULL, NULL);
    pa_timespec_reset(&u->adjust_timestamp);
    
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
        u->automatic = 0;
    } else {
        master_sink = NULL;
        ss = m->core->default_sample_spec;
        u->automatic = 1;
    }

    if ((pa_modargs_get_sample_spec(ma, &ss) < 0)) {
        pa_log("Invalid sample specification.");
        goto fail;
    }

    if (master_sink && ss.channels == master_sink->sample_spec.channels)
        map = master_sink->channel_map;
    else
        pa_channel_map_init_auto(&map, ss.channels, PA_CHANNEL_MAP_DEFAULT);

    if ((pa_modargs_get_channel_map(ma, &map) < 0)) {
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

    u->sink->flags = PA_SINK_CAN_SUSPEND|PA_SINK_LATENCY;
    pa_sink_set_module(u->sink, m);
    pa_sink_set_description(u->sink, "Simultaneous output");

    u->block_size = pa_bytes_per_second(&ss) / 20; /* 50 ms */
    if (u->block_size <= 0)
        u->block_size = pa_frame_size(&ss);
    
    if (!u->automatic) {
        const char*split_state;
        char *n = NULL;
        pa_assert(slaves);

        /* The master and slaves have been specified manually */
        
        if (!(u->master = output_new(u, master_sink, resample_method))) {
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
            
            if (!output_new(u, slave_sink, resample_method)) {
                pa_log("Failed to create slave sink input on sink '%s'.", slave_sink->name);
                goto fail;
            }
        }

        if (pa_idxset_size(u->outputs) <= 1)
            pa_log_warn("WARNING: No slave sinks specified.");

        u->sink_new_slot = NULL;
        
    } else {
        pa_sink *s;

        /* We're in automatic mode, we elect one hw sink to the master
         * and attach all other hw sinks as slaves to it */

        for (s = pa_idxset_first(m->core->sinks, &idx); s; s = pa_idxset_next(m->core->sinks, &idx)) {

            if (!(s->flags & PA_SINK_HARDWARE) || s == u->sink)
                continue;

            if (!output_new(u, s, resample_method)) {
                pa_log("Failed to create sink input on sink '%s'.", s->name);
                goto fail;
            }
        }

        u->sink_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_NEW_POST], (pa_hook_cb_t) sink_new_hook_cb, u);
    }

    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], (pa_hook_cb_t) sink_unlink_hook_cb, u);
    u->sink_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], (pa_hook_cb_t) sink_state_changed_hook_cb, u);
    
    if (pick_master(u) < 0)
        goto fail;

    /* Activate the sink and the sink inputs */
    pa_sink_put(u->sink);
    for (o = pa_idxset_first(u->outputs, &idx); o; o = pa_idxset_next(u->outputs, &idx))
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

    if (o->userdata) {
        if (o->userdata->sink && PA_SINK_LINKED(pa_sink_get_state(o->userdata->sink)))
            pa_asyncmsgq_send(o->userdata->sink->asyncmsgq, PA_MSGOBJECT(o->userdata->sink), SINK_MESSAGE_REMOVE_OUTPUT, o, 0, NULL);
        else
            PA_LLIST_REMOVE(struct output, o->userdata->thread_info.outputs, o);
    }

    pa_assert_se(pa_idxset_remove_by_data(o->userdata->outputs, o, NULL));

    if (o->userdata->master == o) {
        /* Make sure the master points to a different output */
        o->userdata->master = NULL;
        pick_master(o->userdata);
    }
    
    update_description(o->userdata);
    
    if (o->sink_input) {
        pa_sink_input_unlink(o->sink_input);
        pa_sink_input_unref(o->sink_input);
    }

    if (o->rtpoll_item)
        pa_rtpoll_item_free(o->rtpoll_item);

    if (o->memblockq)
        pa_memblockq_free(o->memblockq);

    if (o->asyncmsgq)
        pa_asyncmsgq_unref(o->asyncmsgq);
    
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

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->outputs) {
        while ((o = pa_idxset_first(u->outputs, NULL)))
            output_free(o);
        
        pa_idxset_free(u->outputs, NULL, NULL);
    }

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
    
    pa_mutex_free(u->mutex);
        
    pa_xfree(u);
}


