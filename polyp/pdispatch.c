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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "pdispatch.h"
#include "native-common.h"
#include "xmalloc.h"
#include "llist.h"
#include "log.h"

/*#define DEBUG_OPCODES*/

#ifdef DEBUG_OPCODES

static const char *command_names[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = "ERROR",
    [PA_COMMAND_TIMEOUT] = "TIMEOUT",
    [PA_COMMAND_REPLY] = "REPLY",
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = "CREATE_PLAYBACK_STREAM",
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = "DELETE_PLAYBACK_STREAM",
    [PA_COMMAND_CREATE_RECORD_STREAM] = "CREATE_RECORD_STREAM",
    [PA_COMMAND_DELETE_RECORD_STREAM] = "DELETE_RECORD_STREAM",
    [PA_COMMAND_AUTH] = "AUTH",
    [PA_COMMAND_REQUEST] = "REQUEST",
    [PA_COMMAND_EXIT] = "EXIT",
    [PA_COMMAND_SET_NAME] = "SET_NAME",
    [PA_COMMAND_LOOKUP_SINK] = "LOOKUP_SINK",
    [PA_COMMAND_LOOKUP_SOURCE] = "LOOKUP_SOURCE",
    [PA_COMMAND_DRAIN_PLAYBACK_STREAM] = "DRAIN_PLAYBACK_STREAM",
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = "PLAYBACK_STREAM_KILLED",
    [PA_COMMAND_RECORD_STREAM_KILLED] = "RECORD_STREAM_KILLED",
    [PA_COMMAND_STAT] = "STAT",
    [PA_COMMAND_GET_PLAYBACK_LATENCY] = "PLAYBACK_LATENCY",
    [PA_COMMAND_CREATE_UPLOAD_STREAM] = "CREATE_UPLOAD_STREAM",
    [PA_COMMAND_DELETE_UPLOAD_STREAM] = "DELETE_UPLOAD_STREAM",
    [PA_COMMAND_FINISH_UPLOAD_STREAM] = "FINISH_UPLOAD_STREAM",
    [PA_COMMAND_PLAY_SAMPLE] = "PLAY_SAMPLE",
    [PA_COMMAND_REMOVE_SAMPLE] = "REMOVE_SAMPLE",
    [PA_COMMAND_GET_SERVER_INFO] = "GET_SERVER_INFO",
    [PA_COMMAND_GET_SINK_INFO] = "GET_SINK_INFO",
    [PA_COMMAND_GET_SINK_INFO_LIST] = "GET_SINK_INFO_LIST",
    [PA_COMMAND_GET_SOURCE_INFO] = "GET_SOURCE_INFO",
    [PA_COMMAND_GET_SOURCE_INFO_LIST] = "GET_SOURCE_INFO_LIST",
    [PA_COMMAND_GET_MODULE_INFO] = "GET_MODULE_INFO",
    [PA_COMMAND_GET_MODULE_INFO_LIST] = "GET_MODULE_INFO_LIST",
    [PA_COMMAND_GET_CLIENT_INFO] = "GET_CLIENT_INFO",
    [PA_COMMAND_GET_CLIENT_INFO_LIST] = "GET_CLIENT_INFO_LIST",
    [PA_COMMAND_GET_SAMPLE_INFO] = "GET_SAMPLE_INFO",
    [PA_COMMAND_GET_SAMPLE_INFO_LIST] = "GET_SAMPLE_INFO_LIST",
    [PA_COMMAND_GET_SINK_INPUT_INFO] = "GET_SINK_INPUT_INFO",
    [PA_COMMAND_GET_SINK_INPUT_INFO_LIST] = "GET_SINK_INPUT_INFO_LIST",
    [PA_COMMAND_GET_SOURCE_OUTPUT_INFO] = "GET_SOURCE_OUTPUT_INFO",
    [PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST] = "GET_SOURCE_OUTPUT_INFO_LIST",
    [PA_COMMAND_SUBSCRIBE] = "SUBSCRIBE",
    [PA_COMMAND_SUBSCRIBE_EVENT] = "SUBSCRIBE_EVENT",
    [PA_COMMAND_SET_SINK_VOLUME] = "SET_SINK_VOLUME",
    [PA_COMMAND_SET_SINK_INPUT_VOLUME] = "SET_SINK_INPUT_VOLUME",
    [PA_COMMAND_TRIGGER_PLAYBACK_STREAM] = "TRIGGER_PLAYBACK_STREAM",
    [PA_COMMAND_FLUSH_PLAYBACK_STREAM] = "FLUSH_PLAYBACK_STREAM",
    [PA_COMMAND_CORK_PLAYBACK_STREAM] = "CORK_PLAYBACK_STREAM",
};

#endif

struct reply_info {
    struct pa_pdispatch *pdispatch;
    PA_LLIST_FIELDS(struct reply_info);
    void (*callback)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
    void *userdata;
    uint32_t tag;
    struct pa_time_event *time_event;
};

struct pa_pdispatch {
    int ref;
    struct pa_mainloop_api *mainloop;
    const struct pa_pdispatch_command *command_table;
    unsigned n_commands;
    PA_LLIST_HEAD(struct reply_info, replies);
    void (*drain_callback)(struct pa_pdispatch *pd, void *userdata);
    void *drain_userdata;
};

static void reply_info_free(struct reply_info *r) {
    assert(r && r->pdispatch && r->pdispatch->mainloop);

    if (r->time_event)
        r->pdispatch->mainloop->time_free(r->time_event);
    
    PA_LLIST_REMOVE(struct reply_info, r->pdispatch->replies, r);
    
    pa_xfree(r);
}

struct pa_pdispatch* pa_pdispatch_new(struct pa_mainloop_api *mainloop, const struct pa_pdispatch_command*table, unsigned entries) {
    struct pa_pdispatch *pd;
    assert(mainloop);

    assert((entries && table) || (!entries && !table));
    
    pd = pa_xmalloc(sizeof(struct pa_pdispatch));
    pd->ref = 1;
    pd->mainloop = mainloop;
    pd->command_table = table;
    pd->n_commands = entries;
    PA_LLIST_HEAD_INIT(struct pa_reply_info, pd->replies);
    pd->drain_callback = NULL;
    pd->drain_userdata = NULL;

    return pd;
}

void pdispatch_free(struct pa_pdispatch *pd) {
    assert(pd);

    while (pd->replies)
        reply_info_free(pd->replies);
    
    pa_xfree(pd);
}

static void run_action(struct pa_pdispatch *pd, struct reply_info *r, uint32_t command, struct pa_tagstruct *ts) {
    void (*callback)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
    void *userdata;
    uint32_t tag;
    assert(r);

    pa_pdispatch_ref(pd);
    
    callback = r->callback;
    userdata = r->userdata;
    tag = r->tag;
    
    reply_info_free(r);
    
    callback(pd, command, tag, ts, userdata);

    if (pd->drain_callback && !pa_pdispatch_is_pending(pd))
        pd->drain_callback(pd, pd->drain_userdata);

    pa_pdispatch_unref(pd);
}

int pa_pdispatch_run(struct pa_pdispatch *pd, struct pa_packet*packet, void *userdata) {
    uint32_t tag, command;
    struct pa_tagstruct *ts = NULL;
    int ret = -1;
    assert(pd && packet && packet->data);

    pa_pdispatch_ref(pd);
    
    if (packet->length <= 8)
        goto finish;

    ts = pa_tagstruct_new(packet->data, packet->length);
    assert(ts);
    
    if (pa_tagstruct_getu32(ts, &command) < 0 ||
        pa_tagstruct_getu32(ts, &tag) < 0)
        goto finish;

#ifdef DEBUG_OPCODES
{
    char t[256];
    char const *p;
    if (!(p = command_names[command]))
        snprintf((char*) (p = t), sizeof(t), "%u", command);
        
    pa_log(__FILE__": Recieved opcode <%s>\n", p);
}
#endif

    if (command == PA_COMMAND_ERROR || command == PA_COMMAND_REPLY) {
        struct reply_info *r;

        for (r = pd->replies; r; r = r->next)
            if (r->tag == tag)
                break;

        if (r)
            run_action(pd, r, command, ts);

    } else if (pd->command_table && (command < pd->n_commands) && pd->command_table[command].proc) {
        const struct pa_pdispatch_command *c = pd->command_table+command;

        c->proc(pd, command, tag, ts, userdata);
    } else {
        pa_log(__FILE__": Recieved unsupported command %u\n", command);
        goto finish;
    }

    ret = 0;
        
finish:
    if (ts)
        pa_tagstruct_free(ts);

    pa_pdispatch_unref(pd);

    return ret;
}

static void timeout_callback(struct pa_mainloop_api*m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct reply_info*r = userdata;
    assert(r && r->time_event == e && r->pdispatch && r->pdispatch->mainloop == m && r->callback);

    run_action(r->pdispatch, r, PA_COMMAND_TIMEOUT, NULL);
}

void pa_pdispatch_register_reply(struct pa_pdispatch *pd, uint32_t tag, int timeout, void (*cb)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void *userdata) {
    struct reply_info *r;
    struct timeval tv;
    assert(pd && pd->ref >= 1 && cb);

    r = pa_xmalloc(sizeof(struct reply_info));
    r->pdispatch = pd;
    r->callback = cb;
    r->userdata = userdata;
    r->tag = tag;
    
    gettimeofday(&tv, NULL);
    tv.tv_sec += timeout;

    r->time_event = pd->mainloop->time_new(pd->mainloop, &tv, timeout_callback, r);
    assert(r->time_event);

    PA_LLIST_PREPEND(struct reply_info, pd->replies, r);
}

int pa_pdispatch_is_pending(struct pa_pdispatch *pd) {
    assert(pd);

    return !!pd->replies;
}

void pa_pdispatch_set_drain_callback(struct pa_pdispatch *pd, void (*cb)(struct pa_pdispatch *pd, void *userdata), void *userdata) {
    assert(pd);
    assert(!cb || pa_pdispatch_is_pending(pd));

    pd->drain_callback = cb;
    pd->drain_userdata = userdata;
}

void pa_pdispatch_unregister_reply(struct pa_pdispatch *pd, void *userdata) {
    struct reply_info *r, *n;
    assert(pd);

    for (r = pd->replies; r; r = n) {
        n = r->next;

        if (r->userdata == userdata) 
            reply_info_free(r);
    }
}

void pa_pdispatch_unref(struct pa_pdispatch *pd) {
    assert(pd && pd->ref >= 1);

    if (!(--(pd->ref)))
        pdispatch_free(pd);
}

struct pa_pdispatch* pa_pdispatch_ref(struct pa_pdispatch *pd) {
    assert(pd && pd->ref >= 1);
    pd->ref++;
    return pd;
}
