#ifndef foopolyplibinternalhfoo
#define foopolyplibinternalhfoo

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

#include "mainloop-api.h"
#include "socket-client.h"
#include "pstream.h"
#include "pdispatch.h"
#include "dynarray.h"

#include "polyplib-context.h"
#include "polyplib-stream.h"
#include "polyplib-operation.h"
#include "llist.h"
#include "native-common.h"
#include "client-conf.h"
#include "strlist.h"
#include "mcalign.h"

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
    
    uint32_t ctag;
    uint32_t error;
    pa_context_state state;
    
    void (*state_callback)(pa_context*c, void *userdata);
    void *state_userdata;

    void (*subscribe_callback)(pa_context *c, pa_subscription_event_type t, uint32_t idx, void *userdata);
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

struct pa_stream {
    int ref;
    pa_context *context;
    pa_mainloop_api *mainloop;
    PA_LLIST_FIELDS(pa_stream);

    char *name;
    pa_buffer_attr buffer_attr;
    pa_sample_spec sample_spec;
    uint32_t channel;
    int channel_valid;
    uint32_t device_index;
    pa_stream_direction direction;
    uint32_t requested_bytes;
    uint64_t counter;
    pa_usec_t previous_time;
    pa_usec_t previous_ipol_time;
    pa_stream_state state;
    pa_mcalign *mcalign;

    int interpolate;
    int corked;

    uint32_t ipol_usec;
    struct timeval ipol_timestamp;
    pa_time_event *ipol_event;
    int ipol_requested;
    
    void (*state_callback)(pa_stream*c, void *userdata);
    void *state_userdata;

    void (*read_callback)(pa_stream *p, const void*data, size_t length, void *userdata);
    void *read_userdata;

    void (*write_callback)(pa_stream *p, size_t length, void *userdata);
    void *write_userdata;
};

typedef void (*pa_operation_callback)(void);

struct pa_operation {
    int ref;
    pa_context *context;
    pa_stream *stream;
    PA_LLIST_FIELDS(pa_operation);

    pa_operation_state state;
    void *userdata;
    pa_operation_callback callback;
};

void pa_command_request(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_stream_killed(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_command_subscribe_event(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

pa_operation *pa_operation_new(pa_context *c, pa_stream *s);
void pa_operation_done(pa_operation *o);

void pa_create_stream_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_stream_disconnect_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_context_simple_ack_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
void pa_stream_simple_ack_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

void pa_context_fail(pa_context *c, int error);
void pa_context_set_state(pa_context *c, pa_context_state st);
int pa_context_handle_error(pa_context *c, uint32_t command, pa_tagstruct *t);
pa_operation* pa_context_send_simple_command(pa_context *c, uint32_t command, void (*internal_callback)(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata), void (*cb)(void), void *userdata);

void pa_stream_set_state(pa_stream *s, pa_stream_state st);

void pa_stream_trash_ipol(pa_stream *s);


#endif
