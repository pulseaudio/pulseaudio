#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "pdispatch.h"
#include "protocol-native-spec.h"

struct reply_info {
    struct pa_pdispatch *pdispatch;
    struct reply_info *next, *previous;
    int (*callback)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
    void *userdata;
    uint32_t tag;
    void *mainloop_timeout;
};

struct pa_pdispatch {
    struct pa_mainloop_api *mainloop;
    const struct pa_pdispatch_command *command_table;
    unsigned n_commands;
    struct reply_info *replies;
};

static void reply_info_free(struct reply_info *r) {
    assert(r && r->pdispatch && r->pdispatch->mainloop);

    if (r->pdispatch)
        r->pdispatch->mainloop->cancel_time(r->pdispatch->mainloop, r->mainloop_timeout);

    if (r->previous)
        r->previous->next = r->next;
    else
        r->pdispatch->replies = r->next;

    if (r->next)
        r->next->previous = r->previous;
    
    free(r);
}

struct pa_pdispatch* pa_pdispatch_new(struct pa_mainloop_api *mainloop, const struct pa_pdispatch_command*table, unsigned entries) {
    struct pa_pdispatch *pd;
    assert(mainloop);

    assert((entries && table) || (!entries && !table));
    
    pd = malloc(sizeof(struct pa_pdispatch));
    assert(pd);
    pd->mainloop = mainloop;
    pd->command_table = table;
    pd->n_commands = entries;
    pd->replies = NULL;
    return pd;
}

void pa_pdispatch_free(struct pa_pdispatch *pd) {
    assert(pd);
    while (pd->replies)
        reply_info_free(pd->replies);
    free(pd);
}

int pa_pdispatch_run(struct pa_pdispatch *pd, struct pa_packet*packet, void *userdata) {
    uint32_t tag, command;
    assert(pd && packet);
    struct pa_tagstruct *ts = NULL;
    assert(pd && packet && packet->data);

    if (packet->length <= 8)
        goto fail;

    ts = pa_tagstruct_new(packet->data, packet->length);
    assert(ts);
    
    if (pa_tagstruct_getu32(ts, &command) < 0 ||
        pa_tagstruct_getu32(ts, &tag) < 0)
        goto fail;

    if (command == PA_COMMAND_ERROR || command == PA_COMMAND_REPLY) {
        struct reply_info *r;
        int done = 0;

        for (r = pd->replies; r; r = r->next) {
            if (r->tag == tag) {
                int ret = r->callback(r->pdispatch, command, tag, ts, r->userdata);
                reply_info_free(r);
                
                if (ret < 0)
                    goto fail;
                
                done = 1;
                break;
            }
        }

        if (!done)
            goto fail;

    } else if (pd->command_table && command < pd->n_commands) {
        const struct pa_pdispatch_command *c = pd->command_table+command;

        if (!c->proc)
            goto fail;
        
        if (c->proc(pd, command, tag, ts, userdata) < 0)
            goto fail;
    } else
        goto fail;
    
    pa_tagstruct_free(ts);    
        
    return 0;

fail:
    if (ts)
        pa_tagstruct_free(ts);    

    return -1;
}

static void timeout_callback(struct pa_mainloop_api*m, void *id, const struct timeval *tv, void *userdata) {
    struct reply_info*r = userdata;
    assert (r && r->mainloop_timeout == id && r->pdispatch && r->pdispatch->mainloop == m && r->callback);

    r->callback(r->pdispatch, PA_COMMAND_TIMEOUT, r->tag, NULL, r->userdata);
    reply_info_free(r);
}

void pa_pdispatch_register_reply(struct pa_pdispatch *pd, uint32_t tag, int timeout, int (*cb)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void *userdata) {
    struct reply_info *r;
    struct timeval tv;
    assert(pd && cb);

    r = malloc(sizeof(struct reply_info));
    assert(r);
    r->pdispatch = pd;
    r->callback = cb;
    r->userdata = userdata;
    r->tag = tag;

    gettimeofday(&tv, NULL);
    tv.tv_sec += timeout;

    r->mainloop_timeout = pd->mainloop->source_time(pd->mainloop, &tv, timeout_callback, r);
    assert(r->mainloop_timeout);

    r->previous = NULL;
    r->next = pd->replies;
    if (r->next)
        r->next->previous = r;
    pd->replies = r;
}
