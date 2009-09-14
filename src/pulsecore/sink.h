#ifndef foopulsesinkhfoo
#define foopulsesinkhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

typedef struct pa_sink pa_sink;
typedef struct pa_device_port pa_device_port;

#include <inttypes.h>

#include <pulse/def.h>
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>

#include <pulsecore/core.h>
#include <pulsecore/idxset.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/msgobject.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/card.h>
#include <pulsecore/queue.h>
#include <pulsecore/thread-mq.h>

#define PA_MAX_INPUTS_PER_SINK 32

/* Returns true if sink is linked: registered and accessible from client side. */
static inline pa_bool_t PA_SINK_IS_LINKED(pa_sink_state_t x) {
    return x == PA_SINK_RUNNING || x == PA_SINK_IDLE || x == PA_SINK_SUSPENDED;
}

struct pa_device_port {
    char *name;
    char *description;

    unsigned priority;

    /* .. followed by some implementation specific data */
};

#define PA_DEVICE_PORT_DATA(d) ((void*) ((uint8_t*) d + PA_ALIGN(sizeof(pa_device_port))))

struct pa_sink {
    pa_msgobject parent;

    uint32_t index;
    pa_core *core;

    pa_sink_state_t state;
    pa_sink_flags_t flags;
    pa_suspend_cause_t suspend_cause;

    char *name;
    char *driver;                           /* may be NULL */
    pa_proplist *proplist;

    pa_module *module;                      /* may be NULL */
    pa_card *card;                          /* may be NULL */

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *inputs;
    unsigned n_corked;
    pa_source *monitor_source;

    pa_volume_t base_volume; /* shall be constant */
    unsigned n_volume_steps; /* shall be constant */

    /* Also see http://pulseaudio.org/wiki/InternalVolumes */
    pa_cvolume reference_volume; /* The volume exported and taken as reference base for relative sink input volumes */
    pa_cvolume real_volume;      /* The volume that the hardware is configured to  */
    pa_cvolume soft_volume;      /* The internal software volume we apply to all PCM data while it passes through */

    pa_bool_t muted:1;

    pa_bool_t refresh_volume:1;
    pa_bool_t refresh_muted:1;
    pa_bool_t save_port:1;
    pa_bool_t save_volume:1;
    pa_bool_t save_muted:1;

    pa_asyncmsgq *asyncmsgq;

    pa_memchunk silence;

    pa_hashmap *ports;
    pa_device_port *active_port;

    unsigned priority;

    /* Called when the main loop requests a state change. Called from
     * main loop context. If returns -1 the state change will be
     * inhibited */
    int (*set_state)(pa_sink *s, pa_sink_state_t state); /* may be NULL */

    /* Callled when the volume is queried. Called from main loop
     * context. If this is NULL a PA_SINK_MESSAGE_GET_VOLUME message
     * will be sent to the IO thread instead. If refresh_volume is
     * FALSE neither this function is called nor a message is sent. */
    void (*get_volume)(pa_sink *s);             /* may be NULL */

    /* Called when the volume shall be changed. Called from main loop
     * context. If this is NULL a PA_SINK_MESSAGE_SET_VOLUME message
     * will be sent to the IO thread instead. */
    void (*set_volume)(pa_sink *s);             /* dito */

    /* Called when the mute setting is queried. Called from main loop
     * context. If this is NULL a PA_SINK_MESSAGE_GET_MUTE message
     * will be sent to the IO thread instead. If refresh_mute is
     * FALSE neither this function is called nor a message is sent.*/
    void (*get_mute)(pa_sink *s);               /* dito */

    /* Called when the mute setting shall be changed. Called from main
     * loop context. If this is NULL a PA_SINK_MESSAGE_SET_MUTE
     * message will be sent to the IO thread instead. */
    void (*set_mute)(pa_sink *s);               /* dito */

    /* Called when a rewind request is issued. Called from IO thread
     * context. */
    void (*request_rewind)(pa_sink *s);        /* dito */

    /* Called when a the requested latency is changed. Called from IO
     * thread context. */
    void (*update_requested_latency)(pa_sink *s); /* dito */

    /* Called whenever the port shall be changed. Called from main
     * thread. */
    int (*set_port)(pa_sink *s, pa_device_port *port); /* dito */

    /* Contains copies of the above data so that the real-time worker
     * thread can work without access locking */
    struct {
        pa_sink_state_t state;
        pa_hashmap *inputs;

        pa_rtpoll *rtpoll;

        pa_cvolume soft_volume;
        pa_bool_t soft_muted:1;

        /* The requested latency is used for dynamic latency
         * sinks. For fixed latency sinks it is always identical to
         * the fixed_latency. See below. */
        pa_bool_t requested_latency_valid:1;
        pa_usec_t requested_latency;

        /* The number of bytes streams need to keep around as history to
         * be able to satisfy every DMA buffer rewrite */
        size_t max_rewind;

        /* The number of bytes streams need to keep around to satisfy
         * every DMA write request */
        size_t max_request;

        /* Maximum of what clients requested to rewind in this cycle */
        size_t rewind_nbytes;
        pa_bool_t rewind_requested;

        /* Both dynamic and fixed latencies will be clamped to this
         * range. */
        pa_usec_t min_latency; /* we won't go below this latency */
        pa_usec_t max_latency; /* An upper limit for the latencies */

        /* 'Fixed' simply means that the latency is exclusively
         * decided on by the sink, and the clients have no influence
         * in changing it */
        pa_usec_t fixed_latency; /* for sinks with PA_SINK_DYNAMIC_LATENCY this is 0 */
    } thread_info;

    void *userdata;
};

PA_DECLARE_PUBLIC_CLASS(pa_sink);
#define PA_SINK(s) (pa_sink_cast(s))

typedef enum pa_sink_message {
    PA_SINK_MESSAGE_ADD_INPUT,
    PA_SINK_MESSAGE_REMOVE_INPUT,
    PA_SINK_MESSAGE_GET_VOLUME,
    PA_SINK_MESSAGE_SET_VOLUME,
    PA_SINK_MESSAGE_SYNC_VOLUMES,
    PA_SINK_MESSAGE_GET_MUTE,
    PA_SINK_MESSAGE_SET_MUTE,
    PA_SINK_MESSAGE_GET_LATENCY,
    PA_SINK_MESSAGE_GET_REQUESTED_LATENCY,
    PA_SINK_MESSAGE_SET_STATE,
    PA_SINK_MESSAGE_START_MOVE,
    PA_SINK_MESSAGE_FINISH_MOVE,
    PA_SINK_MESSAGE_ATTACH,
    PA_SINK_MESSAGE_DETACH,
    PA_SINK_MESSAGE_SET_LATENCY_RANGE,
    PA_SINK_MESSAGE_GET_LATENCY_RANGE,
    PA_SINK_MESSAGE_SET_FIXED_LATENCY,
    PA_SINK_MESSAGE_GET_FIXED_LATENCY,
    PA_SINK_MESSAGE_GET_MAX_REWIND,
    PA_SINK_MESSAGE_GET_MAX_REQUEST,
    PA_SINK_MESSAGE_SET_MAX_REWIND,
    PA_SINK_MESSAGE_SET_MAX_REQUEST,
    PA_SINK_MESSAGE_MAX
} pa_sink_message_t;

typedef struct pa_sink_new_data {
    char *name;
    pa_proplist *proplist;

    const char *driver;
    pa_module *module;
    pa_card *card;

    pa_hashmap *ports;
    char *active_port;

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_cvolume volume;
    pa_bool_t muted :1;

    pa_bool_t sample_spec_is_set:1;
    pa_bool_t channel_map_is_set:1;
    pa_bool_t volume_is_set:1;
    pa_bool_t muted_is_set:1;

    pa_bool_t namereg_fail:1;

    pa_bool_t save_port:1;
    pa_bool_t save_volume:1;
    pa_bool_t save_muted:1;
} pa_sink_new_data;

pa_sink_new_data* pa_sink_new_data_init(pa_sink_new_data *data);
void pa_sink_new_data_set_name(pa_sink_new_data *data, const char *name);
void pa_sink_new_data_set_sample_spec(pa_sink_new_data *data, const pa_sample_spec *spec);
void pa_sink_new_data_set_channel_map(pa_sink_new_data *data, const pa_channel_map *map);
void pa_sink_new_data_set_volume(pa_sink_new_data *data, const pa_cvolume *volume);
void pa_sink_new_data_set_muted(pa_sink_new_data *data, pa_bool_t mute);
void pa_sink_new_data_set_port(pa_sink_new_data *data, const char *port);
void pa_sink_new_data_done(pa_sink_new_data *data);

/*** To be called exclusively by the sink driver, from main context */

pa_sink* pa_sink_new(
        pa_core *core,
        pa_sink_new_data *data,
        pa_sink_flags_t flags);

void pa_sink_put(pa_sink *s);
void pa_sink_unlink(pa_sink* s);

void pa_sink_set_description(pa_sink *s, const char *description);
void pa_sink_set_asyncmsgq(pa_sink *s, pa_asyncmsgq *q);
void pa_sink_set_rtpoll(pa_sink *s, pa_rtpoll *p);

void pa_sink_set_max_rewind(pa_sink *s, size_t max_rewind);
void pa_sink_set_max_request(pa_sink *s, size_t max_request);
void pa_sink_set_latency_range(pa_sink *s, pa_usec_t min_latency, pa_usec_t max_latency);
void pa_sink_set_fixed_latency(pa_sink *s, pa_usec_t latency);

void pa_sink_detach(pa_sink *s);
void pa_sink_attach(pa_sink *s);

void pa_sink_set_soft_volume(pa_sink *s, const pa_cvolume *volume);
void pa_sink_volume_changed(pa_sink *s, const pa_cvolume *new_volume);
void pa_sink_mute_changed(pa_sink *s, pa_bool_t new_muted);

void pa_sink_update_flags(pa_sink *s, pa_sink_flags_t mask, pa_sink_flags_t value);

pa_bool_t pa_device_init_description(pa_proplist *p);
pa_bool_t pa_device_init_icon(pa_proplist *p, pa_bool_t is_sink);
pa_bool_t pa_device_init_intended_roles(pa_proplist *p);
unsigned pa_device_init_priority(pa_proplist *p);

/**** May be called by everyone, from main context */

/* The returned value is supposed to be in the time domain of the sound card! */
pa_usec_t pa_sink_get_latency(pa_sink *s);
pa_usec_t pa_sink_get_requested_latency(pa_sink *s);
void pa_sink_get_latency_range(pa_sink *s, pa_usec_t *min_latency, pa_usec_t *max_latency);
pa_usec_t pa_sink_get_fixed_latency(pa_sink *s);

size_t pa_sink_get_max_rewind(pa_sink *s);
size_t pa_sink_get_max_request(pa_sink *s);

int pa_sink_update_status(pa_sink*s);
int pa_sink_suspend(pa_sink *s, pa_bool_t suspend, pa_suspend_cause_t cause);
int pa_sink_suspend_all(pa_core *c, pa_bool_t suspend, pa_suspend_cause_t cause);

void pa_sink_set_volume(pa_sink *sink, const pa_cvolume *volume, pa_bool_t sendmsg, pa_bool_t save);
const pa_cvolume *pa_sink_get_volume(pa_sink *sink, pa_bool_t force_refresh);

void pa_sink_set_mute(pa_sink *sink, pa_bool_t mute, pa_bool_t save);
pa_bool_t pa_sink_get_mute(pa_sink *sink, pa_bool_t force_refresh);

pa_bool_t pa_sink_update_proplist(pa_sink *s, pa_update_mode_t mode, pa_proplist *p);

int pa_sink_set_port(pa_sink *s, const char *name, pa_bool_t save);

unsigned pa_sink_linked_by(pa_sink *s); /* Number of connected streams */
unsigned pa_sink_used_by(pa_sink *s); /* Number of connected streams which are not corked */
unsigned pa_sink_check_suspend(pa_sink *s); /* Returns how many streams are active that don't allow suspensions */
#define pa_sink_get_state(s) ((s)->state)

/* Moves all inputs away, and stores them in pa_queue */
pa_queue *pa_sink_move_all_start(pa_sink *s, pa_queue *q);
void pa_sink_move_all_finish(pa_sink *s, pa_queue *q, pa_bool_t save);
void pa_sink_move_all_fail(pa_queue *q);

/*** To be called exclusively by the sink driver, from IO context */

void pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result);
void pa_sink_render_full(pa_sink *s, size_t length, pa_memchunk *result);
void pa_sink_render_into(pa_sink*s, pa_memchunk *target);
void pa_sink_render_into_full(pa_sink *s, pa_memchunk *target);

void pa_sink_process_rewind(pa_sink *s, size_t nbytes);

int pa_sink_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk);

void pa_sink_attach_within_thread(pa_sink *s);
void pa_sink_detach_within_thread(pa_sink *s);

pa_usec_t pa_sink_get_requested_latency_within_thread(pa_sink *s);

void pa_sink_set_max_rewind_within_thread(pa_sink *s, size_t max_rewind);
void pa_sink_set_max_request_within_thread(pa_sink *s, size_t max_request);

void pa_sink_set_latency_range_within_thread(pa_sink *s, pa_usec_t min_latency, pa_usec_t max_latency);
void pa_sink_set_fixed_latency_within_thread(pa_sink *s, pa_usec_t latency);

/*** To be called exclusively by sink input drivers, from IO context */

void pa_sink_request_rewind(pa_sink*s, size_t nbytes);

void pa_sink_invalidate_requested_latency(pa_sink *s, pa_bool_t dynamic);

pa_usec_t pa_sink_get_latency_within_thread(pa_sink *s);

pa_device_port *pa_device_port_new(const char *name, const char *description, size_t extra);
void pa_device_port_free(pa_device_port *p);

/* Verify that we called in IO context (aka 'thread context), or that
 * the sink is not yet set up, i.e. the thread not set up yet. See
 * pa_assert_io_context() in thread-mq.h for more information. */
#define pa_sink_assert_io_context(s) \
    pa_assert(pa_thread_mq_get() || !PA_SINK_IS_LINKED((s)->state))

#endif
