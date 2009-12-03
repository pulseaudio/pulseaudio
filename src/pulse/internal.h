#ifndef foointernalhfoo
#define foointernalhfoo

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

#include <pulse/mainloop-api.h>
#include <pulse/context.h>
#include <pulse/stream.h>
#include <pulse/operation.h>
#include <pulse/subscribe.h>
#include <pulse/ext-device-manager.h>
#include <pulse/ext-stream-restore.h>

#include <pulsecore/socket-client.h>
#include <pulsecore/pstream.h>
#include <pulsecore/pdispatch.h>
#include <pulsecore/llist.h>
#include <pulsecore/native-common.h>
#include <pulsecore/strlist.h>
#include <pulsecore/mcalign.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/time-smoother.h>
#ifdef HAVE_DBUS
#include <pulsecore/dbus-util.h>
#endif

#include "client-conf.h"

#define DEFAULT_TIMEOUT (30)

struct pa_context {
    PA_REFCNT_DECLARE;

#ifdef HAVE_DBUS
    pa_dbus_wrap_connection *system_bus;
    pa_dbus_wrap_connection *session_bus;
#endif

    pa_proplist *proplist;
    pa_mainloop_api* mainloop;

    pa_socket_client *client;
    pa_pstream *pstream;
    pa_pdispatch *pdispatch;

    pa_hashmap *record_streams, *playback_streams;
    PA_LLIST_HEAD(pa_stream, streams);
    PA_LLIST_HEAD(pa_operation, operations);

    uint32_t version;
    uint32_t ctag;
    uint32_t csyncid;
    int error;
    pa_context_state_t state;

    pa_context_notify_cb_t state_callback;
    void *state_userdata;
    pa_context_subscribe_cb_t subscribe_callback;
    void *subscribe_userdata;
    pa_context_event_cb_t event_callback;
    void *event_userdata;

    pa_mempool *mempool;

    pa_bool_t is_local:1;
    pa_bool_t do_shm:1;
    pa_bool_t server_specified:1;
    pa_bool_t no_fail:1;
    pa_bool_t do_autospawn:1;
    pa_bool_t use_rtclock:1;
    pa_bool_t filter_added:1;
    pa_spawn_api spawn_api;

    pa_strlist *server_list;

    char *server;

    pa_client_conf *conf;

    uint32_t client_index;

    /* Extension specific data */
    struct {
        pa_ext_device_manager_subscribe_cb_t callback;
        void *userdata;
    } ext_device_manager;
    struct {
        pa_ext_stream_restore_subscribe_cb_t callback;
        void *userdata;
    } ext_stream_restore;
};

#define PA_MAX_WRITE_INDEX_CORRECTIONS 32

typedef struct pa_index_correction {
    uint32_t tag;
    int64_t value;
    pa_bool_t valid:1;
    pa_bool_t absolute:1;
    pa_bool_t corrupt:1;
} pa_index_correction;

struct pa_stream {
    PA_REFCNT_DECLARE;
    PA_LLIST_FIELDS(pa_stream);

    pa_context *context;
    pa_mainloop_api *mainloop;

    uint32_t direct_on_input;

    pa_stream_direction_t direction;
    pa_stream_state_t state;
    pa_stream_flags_t flags;

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_proplist *proplist;

    pa_bool_t channel_valid:1;
    pa_bool_t suspended:1;
    pa_bool_t corked:1;
    pa_bool_t timing_info_valid:1;
    pa_bool_t auto_timing_update_requested:1;

    uint32_t channel;
    uint32_t syncid;
    uint32_t stream_index;

    int64_t requested_bytes;
    pa_buffer_attr buffer_attr;

    uint32_t device_index;
    char *device_name;

    /* playback */
    pa_memblock *write_memblock;
    void *write_data;

    /* recording */
    pa_memchunk peek_memchunk;
    void *peek_data;
    pa_memblockq *record_memblockq;

    /* Store latest latency info */
    pa_timing_info timing_info;

    /* Use to make sure that time advances monotonically */
    pa_usec_t previous_time;

    /* time updates with tags older than these are invalid */
    uint32_t write_index_not_before;
    uint32_t read_index_not_before;

    /* Data about individual timing update corrections */
    pa_index_correction write_index_corrections[PA_MAX_WRITE_INDEX_CORRECTIONS];
    int current_write_index_correction;

    /* Latency interpolation stuff */
    pa_time_event *auto_timing_update_event;
    pa_usec_t auto_timing_interval_usec;

    pa_smoother *smoother;

    /* Callbacks */
    pa_stream_notify_cb_t state_callback;
    void *state_userdata;
    pa_stream_request_cb_t read_callback;
    void *read_userdata;
    pa_stream_request_cb_t write_callback;
    void *write_userdata;
    pa_stream_notify_cb_t overflow_callback;
    void *overflow_userdata;
    pa_stream_notify_cb_t underflow_callback;
    void *underflow_userdata;
    pa_stream_notify_cb_t latency_update_callback;
    void *latency_update_userdata;
    pa_stream_notify_cb_t moved_callback;
    void *moved_userdata;
    pa_stream_notify_cb_t suspended_callback;
    void *suspended_userdata;
    pa_stream_notify_cb_t started_callback;
    void *started_userdata;
    pa_stream_event_cb_t event_callback;
    void *event_userdata;
    pa_stream_notify_cb_t buffer_attr_callback;
    void *buffer_attr_userdata;
};

typedef void (*pa_operation_cb_t)(void);

struct pa_operation {
    PA_REFCNT_DECLARE;

    pa_context *context;
    pa_stream *stream;

    PA_LLIST_FIELDS(pa_operation);

    pa_operation_state_t state;
    void *userdata;
    pa_operation_cb_t callback;

    void *private; /* some operations might need this */
};

void pa_command_request(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_killed(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_subscribe_event(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_overflow_or_underflow(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_suspended(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_moved(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_started(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_event(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_client_event(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_buffer_attr(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

pa_operation *pa_operation_new(pa_context *c, pa_stream *s, pa_operation_cb_t callback, void *userdata);
void pa_operation_done(pa_operation *o);

void pa_create_stream_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_stream_disconnect_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_context_simple_ack_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_stream_simple_ack_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

void pa_context_fail(pa_context *c, int error);
int pa_context_set_error(pa_context *c, int error);
void pa_context_set_state(pa_context *c, pa_context_state_t st);
int pa_context_handle_error(pa_context *c, uint32_t command, pa_tagstruct *t, pa_bool_t fail);
pa_operation* pa_context_send_simple_command(pa_context *c, uint32_t command, void (*internal_callback)(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata), void (*cb)(void), void *userdata);

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st);

pa_tagstruct *pa_tagstruct_command(pa_context *c, uint32_t command, uint32_t *tag);

#define PA_CHECK_VALIDITY(context, expression, error)         \
    do {                                                      \
        if (!(expression))                                    \
            return -pa_context_set_error((context), (error)); \
    } while(FALSE)


#define PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, value) \
    do {                                                                \
        if (!(expression)) {                                            \
            pa_context_set_error((context), (error));                   \
            return value;                                               \
        }                                                               \
    } while(FALSE)

#define PA_CHECK_VALIDITY_RETURN_NULL(context, expression, error)       \
    PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, NULL)

#define PA_FAIL(context, error)                                 \
    do {                                                        \
        return -pa_context_set_error((context), (error));       \
    } while(FALSE)

#define PA_FAIL_RETURN_ANY(context, error, value)      \
    do {                                               \
        pa_context_set_error((context), (error));      \
        return value;                                  \
    } while(FALSE)

#define PA_FAIL_RETURN_NULL(context, error)     \
    PA_FAIL_RETURN_ANY(context, error, NULL)

void pa_ext_device_manager_command(pa_context *c, uint32_t tag, pa_tagstruct *t);
void pa_ext_stream_restore_command(pa_context *c, uint32_t tag, pa_tagstruct *t);

pa_bool_t pa_mainloop_is_our_api(pa_mainloop_api*m);

#endif
