#ifndef foopolyplibinternalhfoo
#define foopolyplibinternalhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
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

#define DEFAULT_TLENGTH (44100*2*2/10)  //(10240*8)
#define DEFAULT_MAXLENGTH ((DEFAULT_TLENGTH*3)/2)
#define DEFAULT_MINREQ 512
#define DEFAULT_PREBUF (DEFAULT_TLENGTH-DEFAULT_MINREQ)
#define DEFAULT_FRAGSIZE 1024

#define DEFAULT_TIMEOUT (10)

#define ENV_AUTOSPAWNED "POLYP_AUTOSPAWNED"

struct pa_context {
    int ref;
    
    char *name;
    struct pa_mainloop_api* mainloop;

    struct pa_socket_client *client;
    struct pa_pstream *pstream;
    struct pa_pdispatch *pdispatch;

    struct pa_dynarray *record_streams, *playback_streams;
    PA_LLIST_HEAD(struct pa_stream, streams);
    PA_LLIST_HEAD(struct pa_operation, operations);
    
    uint32_t ctag;
    uint32_t error;
    enum pa_context_state state;
    
    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];
    
    void (*state_callback)(struct pa_context*c, void *userdata);
    void *state_userdata;

    void (*subscribe_callback)(struct pa_context *c, enum pa_subscription_event_type t, uint32_t index, void *userdata);
    void *subscribe_userdata;

    struct pa_memblock_stat *memblock_stat;

    int local;

    struct pa_client_conf *conf;
};

struct pa_stream {
    int ref;
    struct pa_context *context;
    PA_LLIST_FIELDS(struct pa_stream);

    char *name;
    struct pa_buffer_attr buffer_attr;
    struct pa_sample_spec sample_spec;
    uint32_t channel;
    int channel_valid;
    uint32_t device_index;
    enum pa_stream_direction direction;
    uint32_t requested_bytes;
    uint64_t counter;
    pa_usec_t previous_time;
    enum pa_stream_state state;

    void (*state_callback)(struct pa_stream*c, void *userdata);
    void *state_userdata;

    void (*read_callback)(struct pa_stream *p, const void*data, size_t length, void *userdata);
    void *read_userdata;

    void (*write_callback)(struct pa_stream *p, size_t length, void *userdata);
    void *write_userdata;
};

struct pa_operation {
    int ref;
    struct pa_context *context;
    struct pa_stream *stream;
    PA_LLIST_FIELDS(struct pa_operation);

    enum pa_operation_state state;
    void *userdata;
    void (*callback)();
};

void pa_command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
void pa_command_stream_killed(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
void pa_command_subscribe_event(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

struct pa_operation *pa_operation_new(struct pa_context *c, struct pa_stream *s);
void pa_operation_done(struct pa_operation *o);

void pa_create_stream_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
void pa_stream_disconnect_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
void pa_context_simple_ack_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
void pa_stream_simple_ack_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

void pa_context_fail(struct pa_context *c, int error);
void pa_context_set_state(struct pa_context *c, enum pa_context_state st);
int pa_context_handle_error(struct pa_context *c, uint32_t command, struct pa_tagstruct *t);
struct pa_operation* pa_context_send_simple_command(struct pa_context *c, uint32_t command, void (*internal_callback)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void (*cb)(), void *userdata);

void pa_stream_set_state(struct pa_stream *s, enum pa_stream_state st);

#endif
