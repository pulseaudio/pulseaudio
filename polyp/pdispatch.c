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
};

#endif

struct reply_info {
    struct pa_pdispatch *pdispatch;
    struct reply_info *next, *previous;
    void (*callback)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
    void *userdata;
    uint32_t tag;
    struct pa_time_event *time_event;
    int callback_is_running;
};

struct pa_pdispatch {
    struct pa_mainloop_api *mainloop;
    const struct pa_pdispatch_command *command_table;
    unsigned n_commands;
    struct reply_info *replies;
    void (*drain_callback)(struct pa_pdispatch *pd, void *userdata);
    void *drain_userdata;
    int in_use, shall_free;
};

static void reply_info_free(struct reply_info *r) {
    assert(r && r->pdispatch && r->pdispatch->mainloop);

    if (r->pdispatch)
        r->pdispatch->mainloop->time_free(r->time_event);

    if (r->previous)
        r->previous->next = r->next;
    else
        r->pdispatch->replies = r->next;

    if (r->next)
        r->next->previous = r->previous;
    
    pa_xfree(r);
}

struct pa_pdispatch* pa_pdispatch_new(struct pa_mainloop_api *mainloop, const struct pa_pdispatch_command*table, unsigned entries) {
    struct pa_pdispatch *pd;
    assert(mainloop);

    assert((entries && table) || (!entries && !table));
    
    pd = pa_xmalloc(sizeof(struct pa_pdispatch));
    pd->mainloop = mainloop;
    pd->command_table = table;
    pd->n_commands = entries;
    pd->replies = NULL;
    pd->drain_callback = NULL;
    pd->drain_userdata = NULL;

    pd->in_use = pd->shall_free = 0;

    return pd;
}

void pa_pdispatch_free(struct pa_pdispatch *pd) {
    assert(pd);

    if (pd->in_use) {
        pd->shall_free = 1;
        return;
    }
    
    while (pd->replies)
        reply_info_free(pd->replies);
    pa_xfree(pd);
}

int pa_pdispatch_run(struct pa_pdispatch *pd, struct pa_packet*packet, void *userdata) {
    uint32_t tag, command;
    struct pa_tagstruct *ts = NULL;
    int ret = -1;
    assert(pd && packet && packet->data && !pd->in_use);

    if (packet->length <= 8)
        goto finish;

    ts = pa_tagstruct_new(packet->data, packet->length);
    assert(ts);
    
    if (pa_tagstruct_getu32(ts, &command) < 0 ||
        pa_tagstruct_getu32(ts, &tag) < 0)
        goto finish;

#ifdef DEBUG_OPCODES
    fprintf(stderr, __FILE__": Recieved opcode <%s>\n", command_names[command]);
#endif

    if (command == PA_COMMAND_ERROR || command == PA_COMMAND_REPLY) {
        struct reply_info *r;

        for (r = pd->replies; r; r = r->next)
            if (r->tag == tag)
                break;

        if (r) {
            pd->in_use = r->callback_is_running = 1;
            assert(r->callback);
            r->callback(r->pdispatch, command, tag, ts, r->userdata);
            pd->in_use = r->callback_is_running = 0;
            reply_info_free(r);
            
            if (pd->shall_free)
                pa_pdispatch_free(pd);
            else {
                if (pd->drain_callback && !pa_pdispatch_is_pending(pd))
                    pd->drain_callback(pd, pd->drain_userdata);
            }
        }

    } else if (pd->command_table && command < pd->n_commands) {
        const struct pa_pdispatch_command *c = pd->command_table+command;

        if (c->proc)
            c->proc(pd, command, tag, ts, userdata);
    } else
        goto finish;

    ret = 0;
        
finish:
    if (ts)
        pa_tagstruct_free(ts);    

    return ret;
}

static void timeout_callback(struct pa_mainloop_api*m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct reply_info*r = userdata;
    assert (r && r->time_event == e && r->pdispatch && r->pdispatch->mainloop == m && r->callback);

    r->callback(r->pdispatch, PA_COMMAND_TIMEOUT, r->tag, NULL, r->userdata);
    reply_info_free(r);

    if (r->pdispatch->drain_callback && !pa_pdispatch_is_pending(r->pdispatch))
        r->pdispatch->drain_callback(r->pdispatch, r->pdispatch->drain_userdata);
}

void pa_pdispatch_register_reply(struct pa_pdispatch *pd, uint32_t tag, int timeout, void (*cb)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void *userdata) {
    struct reply_info *r;
    struct timeval tv;
    assert(pd && cb);

    r = pa_xmalloc(sizeof(struct reply_info));
    r->pdispatch = pd;
    r->callback = cb;
    r->userdata = userdata;
    r->tag = tag;
    r->callback_is_running = 0;
    
    gettimeofday(&tv, NULL);
    tv.tv_sec += timeout;

    r->time_event = pd->mainloop->time_new(pd->mainloop, &tv, timeout_callback, r);
    assert(r->time_event);

    r->previous = NULL;
    r->next = pd->replies;
    if (r->next)
        r->next->previous = r;
    pd->replies = r;
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

        if (!r->callback_is_running && r->userdata == userdata) /* when this item's callback is currently running it is destroyed anyway in the very near future */
            reply_info_free(r);
    }
}
