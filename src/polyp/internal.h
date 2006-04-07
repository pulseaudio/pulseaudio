#ifndef foointernalhfoo
#define foointernalhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <polyp/mainloop-api.h>
#include <polyp/context.h>
#include <polyp/stream.h>
#include <polyp/operation.h>
#include <polyp/subscribe.h>

#include <polypcore/socket-client.h>
#include <polypcore/pstream.h>
#include <polypcore/pdispatch.h>
#include <polypcore/dynarray.h>
#include <polypcore/llist.h>
#include <polypcore/native-common.h>
#include <polypcore/strlist.h>
#include <polypcore/mcalign.h>
#include <polypcore/memblockq.h>
#include <polypcore/hashmap.h>

#include "client-conf.h"

#define DEFAULT_TIMEOUT (10)

struct pa_context {
    int ref;
    
    char *name;
    pa_mainloop_api* mainloop;

    pa_socket_client *client;
    pa_pstream *pstream;
    pa_pdispatch *pdispatch;

    pa_dynarray *record_streams, *playback_streams;
    PA_LLIST_HEAD(pa_stream, streams);
    PA_LLIST_HEAD(pa_operation, operations);

    uint32_t version;
    uint32_t ctag;
    uint32_t csyncid;
    uint32_t error;
    pa_context_state_t state;

    pa_context_notify_cb_t state_callback;
    void *state_userdata;

    pa_context_subscribe_cb_t subscribe_callback;
    void *subscribe_userdata;

    pa_memblock_stat *memblock_stat;

    int local;
    int do_autospawn;
    int autospawn_lock_fd;
    pa_spawn_api spawn_api;
    
    pa_strlist *server_list;

    char *server;

    pa_client_conf *conf;
};

#define PA_MAX_LATENCY_CORRECTIONS 10

typedef struct pa_latency_correction {
    uint32_t tag;
    int valid;
    int64_t value;
    int absolute, corrupt;
} pa_latency_correction;

struct pa_stream {
    int ref;
    pa_context *context;
    pa_mainloop_api *mainloop;
    PA_LLIST_FIELDS(pa_stream);

    char *name;
    pa_buffer_attr buffer_attr;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_stream_flags_t flags;
    uint32_t channel;
    uint32_t syncid;
    int channel_valid;
    uint32_t device_index;
    pa_stream_direction_t direction;
    pa_stream_state_t state;
    
    uint32_t requested_bytes;

    pa_memchunk peek_memchunk;
    pa_memblockq *record_memblockq;

    int corked;

    /* Store latest latency info */
    pa_latency_info latency_info;
    int latency_info_valid;
    
    /* Use to make sure that time advances monotonically */
    pa_usec_t previous_time;
    
    /* Latency correction stuff */
    pa_latency_correction latency_corrections[PA_MAX_LATENCY_CORRECTIONS];
    int idx_latency_correction;

    /* Latency interpolation stuff */
    pa_time_event *ipol_event;
    int ipol_requested;
    pa_usec_t ipol_usec;
    int ipol_usec_valid;
    struct timeval ipol_timestamp;

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
};

typedef void (*pa_operation_cb_t)(void);

struct pa_operation {
    int ref;
    pa_context *context;
    pa_stream *stream;
    PA_LLIST_FIELDS(pa_operation);

    pa_operation_state_t state;
    void *userdata;
    pa_operation_cb_t callback;
};

void pa_command_request(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_killed(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_subscribe_event(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_overflow_or_underflow(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

pa_operation *pa_operation_new(pa_context *c, pa_stream *s, pa_operation_cb_t callback, void *userdata);
void pa_operation_done(pa_operation *o);

void pa_create_stream_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_stream_disconnect_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_context_simple_ack_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_stream_simple_ack_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

void pa_context_fail(pa_context *c, int error);
int pa_context_set_error(pa_context *c, int error);
void pa_context_set_state(pa_context *c, pa_context_state_t st);
int pa_context_handle_error(pa_context *c, uint32_t command, pa_tagstruct *t);
pa_operation* pa_context_send_simple_command(pa_context *c, uint32_t command, void (*internal_callback)(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata), void (*cb)(void), void *userdata);

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st);

void pa_stream_trash_ipol(pa_stream *s);

pa_tagstruct *pa_tagstruct_command(pa_context *c, uint32_t command, uint32_t *tag);

#define PA_CHECK_VALIDITY(context, expression, error) do { \
        if (!(expression)) \
            return -pa_context_set_error((context), (error)); \
} while(0)


#define PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, value) do { \
        if (!(expression)) { \
            pa_context_set_error((context), (error)); \
            return value; \
        } \
} while(0)

#define PA_CHECK_VALIDITY_RETURN_NULL(context, expression, error) PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, NULL)


#endif
