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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/wait.h>

#include "polyplib-internal.h"
#include "polyplib-context.h"
#include "native-common.h"
#include "pdispatch.h"
#include "pstream.h"
#include "dynarray.h"
#include "socket-client.h"
#include "pstream-util.h"
#include "authkey.h"
#include "util.h"
#include "xmalloc.h"
#include "log.h"
#include "client-conf.h"

#define DEFAULT_SERVER "/tmp/polypaudio/native"

static const struct pa_pdispatch_command command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_REQUEST] = { pa_command_request },
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = { pa_command_stream_killed },
    [PA_COMMAND_RECORD_STREAM_KILLED] = { pa_command_stream_killed },
    [PA_COMMAND_SUBSCRIBE_EVENT] = { pa_command_subscribe_event },
};

struct pa_context *pa_context_new(struct pa_mainloop_api *mainloop, const char *name) {
    struct pa_context *c;
    assert(mainloop && name);
    
    c = pa_xmalloc(sizeof(struct pa_context));
    c->ref = 1;
    c->name = pa_xstrdup(name);
    c->mainloop = mainloop;
    c->client = NULL;
    c->pstream = NULL;
    c->pdispatch = NULL;
    c->playback_streams = pa_dynarray_new();
    c->record_streams = pa_dynarray_new();
    assert(c->playback_streams && c->record_streams);

    PA_LLIST_HEAD_INIT(struct pa_stream, c->streams);
    PA_LLIST_HEAD_INIT(struct pa_operation, c->operations);
    
    c->error = PA_ERROR_OK;
    c->state = PA_CONTEXT_UNCONNECTED;
    c->ctag = 0;

    c->state_callback = NULL;
    c->state_userdata = NULL;

    c->subscribe_callback = NULL;
    c->subscribe_userdata = NULL;

    c->memblock_stat = pa_memblock_stat_new();
    c->local = -1;
    
    pa_check_signal_is_blocked(SIGPIPE);

    c->conf = pa_client_conf_new();
    pa_client_conf_load(c->conf, NULL);
    pa_client_conf_env(c->conf);
    
    return c;
}

static void context_free(struct pa_context *c) {
    assert(c);

    while (c->operations)
        pa_operation_cancel(c->operations);

    while (c->streams)
        pa_stream_set_state(c->streams, PA_STREAM_TERMINATED);
    
    if (c->client)
        pa_socket_client_unref(c->client);
    if (c->pdispatch)
        pa_pdispatch_unref(c->pdispatch);
    if (c->pstream) {
        pa_pstream_close(c->pstream);
        pa_pstream_unref(c->pstream);
    }
    
    if (c->record_streams)
        pa_dynarray_free(c->record_streams, NULL, NULL);
    if (c->playback_streams)
        pa_dynarray_free(c->playback_streams, NULL, NULL);

    pa_memblock_stat_unref(c->memblock_stat);

    if (c->conf)
        pa_client_conf_free(c->conf);
    
    pa_xfree(c->name);
    pa_xfree(c);
}

struct pa_context* pa_context_ref(struct pa_context *c) {
    assert(c && c->ref >= 1);
    c->ref++;
    return c;
}

void pa_context_unref(struct pa_context *c) {
    assert(c && c->ref >= 1);

    if ((--(c->ref)) == 0)
        context_free(c);
}

void pa_context_set_state(struct pa_context *c, enum pa_context_state st) {
    assert(c);
    
    if (c->state == st)
        return;

    pa_context_ref(c);

    if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
        struct pa_stream *s;
        
        s = c->streams ? pa_stream_ref(c->streams) : NULL;
        while (s) {
            struct pa_stream *n = s->next ? pa_stream_ref(s->next) : NULL;
            pa_stream_set_state(s, st == PA_CONTEXT_FAILED ? PA_STREAM_FAILED : PA_STREAM_TERMINATED);
            pa_stream_unref(s);
            s = n;
        }

        if (c->pdispatch)
            pa_pdispatch_unref(c->pdispatch);
        c->pdispatch = NULL;
    
        if (c->pstream) {
            pa_pstream_close(c->pstream);
            pa_pstream_unref(c->pstream);
        }
        c->pstream = NULL;
    
        if (c->client)
            pa_socket_client_unref(c->client);
        c->client = NULL;
    }

    c->state = st;
    if (c->state_callback)
        c->state_callback(c, c->state_userdata);

    pa_context_unref(c);
}

void pa_context_fail(struct pa_context *c, int error) {
    assert(c);
    c->error = error;
    pa_context_set_state(c, PA_CONTEXT_FAILED);
}

static void pstream_die_callback(struct pa_pstream *p, void *userdata) {
    struct pa_context *c = userdata;
    assert(p && c);
    pa_context_fail(c, PA_ERROR_CONNECTIONTERMINATED);
}

static void pstream_packet_callback(struct pa_pstream *p, struct pa_packet *packet, void *userdata) {
    struct pa_context *c = userdata;
    assert(p && packet && c);

    pa_context_ref(c);
    
    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        pa_log(__FILE__": invalid packet.\n");
        pa_context_fail(c, PA_ERROR_PROTOCOL);
    }

    pa_context_unref(c);
}

static void pstream_memblock_callback(struct pa_pstream *p, uint32_t channel, uint32_t delta, const struct pa_memchunk *chunk, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_stream *s;
    assert(p && chunk && c && chunk->memblock && chunk->memblock->data);

    pa_context_ref(c);
    
    if ((s = pa_dynarray_get(c->record_streams, channel))) {
        if (s->read_callback)
            s->read_callback(s, (uint8_t*) chunk->memblock->data + chunk->index, chunk->length, s->read_userdata);
    }

    pa_context_unref(c);
}

int pa_context_handle_error(struct pa_context *c, uint32_t command, struct pa_tagstruct *t) {
    assert(c && t);

    if (command == PA_COMMAND_ERROR) {
        if (pa_tagstruct_getu32(t, &c->error) < 0) {
            pa_context_fail(c, PA_ERROR_PROTOCOL);
            return -1;
                
        }
    } else if (command == PA_COMMAND_TIMEOUT)
        c->error = PA_ERROR_TIMEOUT;
    else {
        pa_context_fail(c, PA_ERROR_PROTOCOL);
        return -1;
    }

    return 0;
}

static void setup_complete_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    assert(pd && c && (c->state == PA_CONTEXT_AUTHORIZING || c->state == PA_CONTEXT_SETTING_NAME));

    pa_context_ref(c);
    
    if (command != PA_COMMAND_REPLY) {
        
        if (pa_context_handle_error(c, command, t) < 0)
            pa_context_fail(c, PA_ERROR_PROTOCOL);

        pa_context_fail(c, c->error);
        goto finish;
    }

    switch(c->state) {
        case PA_CONTEXT_AUTHORIZING: {
            struct pa_tagstruct *t;
            t = pa_tagstruct_new(NULL, 0);
            assert(t);
            pa_tagstruct_putu32(t, PA_COMMAND_SET_CLIENT_NAME);
            pa_tagstruct_putu32(t, tag = c->ctag++);
            pa_tagstruct_puts(t, c->name);
            pa_pstream_send_tagstruct(c->pstream, t);
            pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, c);

            pa_context_set_state(c, PA_CONTEXT_SETTING_NAME);
            break;
        }

        case PA_CONTEXT_SETTING_NAME :
            pa_context_set_state(c, PA_CONTEXT_READY);
            break;
            
        default:
            assert(0);
    }

finish:
    pa_context_unref(c);
}

static void setup_context(struct pa_context *c, struct pa_iochannel *io) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c && io);

    pa_context_ref(c);
    
    assert(!c->pstream);
    c->pstream = pa_pstream_new(c->mainloop, io, c->memblock_stat);
    assert(c->pstream);
    
    pa_pstream_set_die_callback(c->pstream, pstream_die_callback, c);
    pa_pstream_set_recieve_packet_callback(c->pstream, pstream_packet_callback, c);
    pa_pstream_set_recieve_memblock_callback(c->pstream, pstream_memblock_callback, c);

    assert(!c->pdispatch);
    c->pdispatch = pa_pdispatch_new(c->mainloop, command_table, PA_COMMAND_MAX);
    assert(c->pdispatch);

    if (pa_authkey_load_from_home(PA_NATIVE_COOKIE_FILE, c->auth_cookie, sizeof(c->auth_cookie)) < 0) {
        pa_context_fail(c, PA_ERROR_AUTHKEY);
        goto finish;
    }

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_AUTH);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_put_arbitrary(t, c->auth_cookie, sizeof(c->auth_cookie));
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, c);

    pa_context_set_state(c, PA_CONTEXT_AUTHORIZING);

finish:
    
    pa_context_unref(c);
}

static void on_connection(struct pa_socket_client *client, struct pa_iochannel*io, void *userdata) {
    struct pa_context *c = userdata;
    assert(client && c && c->state == PA_CONTEXT_CONNECTING);

    pa_context_ref(c);
    
    pa_socket_client_unref(client);
    c->client = NULL;

    if (!io) {
        pa_context_fail(c, PA_ERROR_CONNECTIONREFUSED);
        goto finish;
    }

    setup_context(c, io);

finish:
    pa_context_unref(c);
}

static struct sockaddr *resolve_server(const char *server, size_t *len) {
    struct sockaddr *sa;
    struct addrinfo hints, *result = NULL;
    char *port, host[256];
    assert(server && len);

    snprintf(host, sizeof(host), "%s", server);
    host[strcspn(host, ":")] = 0;
    
    if ((port = strrchr(server, ':')))
        port++;
    
    if (!port)
        port = DEFAULT_PORT;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;

    if (getaddrinfo(host, port, &hints, &result) != 0)
        return NULL;
    assert(result);
    
    sa = pa_xmalloc(*len = result->ai_addrlen);
    memcpy(sa, result->ai_addr, *len);

    freeaddrinfo(result);
    
    return sa;
}

static int default_server_is_running(void) {
    struct stat st;
    
    if (DEFAULT_SERVER[0] != '/')
        return 1;

    if (stat(DEFAULT_SERVER, &st) < 0)
        return 0;

    return 1;
}

static int context_connect_spawn(struct pa_context *c, const struct pa_spawn_api *api) {
    pid_t pid;
    int status, r;
    int fds[2] = { -1, -1} ;
    struct pa_iochannel *io;

    pa_context_ref(c);
    
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        pa_log(__FILE__": socketpair() failed: %s\n", strerror(errno));
        pa_context_fail(c, PA_ERROR_INTERNAL);
        goto fail;
    }

    if (api && api->prefork)
        api->prefork();

    if ((pid = fork()) < 0) {
        pa_log(__FILE__": fork() failed: %s\n", strerror(errno));
        pa_context_fail(c, PA_ERROR_INTERNAL);

        if (api && api->postfork)
            api->postfork();
        
        goto fail;
    } else if (!pid) {
        /* Child */
        
        char t[128];
        const char *state = NULL;
#define MAX_ARGS 64
        char *argv[MAX_ARGS+1];
        int n = 0;

        close(fds[0]);
        
        if (api && api->atfork)
            api->atfork();

        snprintf(t, sizeof(t), "%s=1", ENV_AUTOSPAWNED);
        putenv(t);

        argv[n++] = c->conf->daemon_binary;

        snprintf(t, sizeof(t), "-Lmodule-native-protocol-fd fd=%i", fds[1]);
        argv[n++] = pa_xstrdup(t);

        while (n < MAX_ARGS) {
            char *a;

            if (!(a = pa_split_spaces(c->conf->extra_arguments, &state)))
                break;

            argv[n++] = a;
        }

        argv[n++] = NULL;

        execv(argv[0], argv);
        exit(1);
    } 

    /* Parent */

    r = waitpid(pid, &status, 0);

    if (api && api->postfork)
        api->postfork();
        
    if (r < 0) {
        pa_log(__FILE__": waitpid() failed: %s\n", strerror(errno));
        pa_context_fail(c, PA_ERROR_INTERNAL);
        goto fail;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        pa_context_fail(c, PA_ERROR_CONNECTIONREFUSED);
        goto fail;
    }

    close(fds[1]);

    c->local = 1;
    
    io = pa_iochannel_new(c->mainloop, fds[0], fds[0]);
    setup_context(c, io);

    pa_context_unref(c);

    return 0;

fail:
    if (fds[0] != -1)
        close(fds[0]);
    if (fds[1] != -1)
        close(fds[1]);

    pa_context_unref(c);

    return -1;
}

int pa_context_connect(struct pa_context *c, const char *server, int spawn, const struct pa_spawn_api *api) {
    int r = -1;
    assert(c && c->ref >= 1 && c->state == PA_CONTEXT_UNCONNECTED);

    if (!server)
        server = c->conf->default_server;

    if (!server && spawn && c->conf->autospawn && !default_server_is_running())
        return context_connect_spawn(c, api);

    server = DEFAULT_SERVER;

    pa_context_ref(c);

    assert(!c->client);
    
    if (*server == '/') {
        if (!(c->client = pa_socket_client_new_unix(c->mainloop, server))) {
            pa_context_fail(c, PA_ERROR_CONNECTIONREFUSED);
            goto finish;
        }

        c->local = 1;
    } else {
        struct sockaddr* sa;
        size_t sa_len;

        if (!(sa = resolve_server(server, &sa_len))) {
            pa_context_fail(c, PA_ERROR_INVALIDSERVER);
            goto finish;
        }

        c->client = pa_socket_client_new_sockaddr(c->mainloop, sa, sa_len);
        pa_xfree(sa);

        if (!c->client) {
            pa_context_fail(c, PA_ERROR_CONNECTIONREFUSED);
            goto finish;
        }

        c->local = 0;
    }

    pa_socket_client_set_callback(c->client, on_connection, c);
    pa_context_set_state(c, PA_CONTEXT_CONNECTING);

    r = 0;
    
finish:
    pa_context_unref(c);
    
    return r;
}

void pa_context_disconnect(struct pa_context *c) {
    assert(c);
    pa_context_set_state(c, PA_CONTEXT_TERMINATED);
}

enum pa_context_state pa_context_get_state(struct pa_context *c) {
    assert(c && c->ref >= 1);
    return c->state;
}

int pa_context_errno(struct pa_context *c) {
    assert(c && c->ref >= 1);
    return c->error;
}

void pa_context_set_state_callback(struct pa_context *c, void (*cb)(struct pa_context *c, void *userdata), void *userdata) {
    assert(c && c->ref >= 1);
    c->state_callback = cb;
    c->state_userdata = userdata;
}

int pa_context_is_pending(struct pa_context *c) {
    assert(c && c->ref >= 1);

/*     pa_log("pstream: %i\n", pa_pstream_is_pending(c->pstream)); */
/*     pa_log("pdispatch: %i\n", pa_pdispatch_is_pending(c->pdispatch)); */
    
    return (c->pstream && pa_pstream_is_pending(c->pstream)) || (c->pdispatch && pa_pdispatch_is_pending(c->pdispatch)) || c->client;
}

static void set_dispatch_callbacks(struct pa_operation *o);

static void pdispatch_drain_callback(struct pa_pdispatch*pd, void *userdata) {
    set_dispatch_callbacks(userdata);
}

static void pstream_drain_callback(struct pa_pstream *s, void *userdata) {
    set_dispatch_callbacks(userdata);
}

static void set_dispatch_callbacks(struct pa_operation *o) {
    int done = 1;
    assert(o && o->context && o->context->ref >= 1 && o->ref >= 1 && o->context->state == PA_CONTEXT_READY);

    pa_pstream_set_drain_callback(o->context->pstream, NULL, NULL);
    pa_pdispatch_set_drain_callback(o->context->pdispatch, NULL, NULL);
    
    if (pa_pdispatch_is_pending(o->context->pdispatch)) {
        pa_pdispatch_set_drain_callback(o->context->pdispatch, pdispatch_drain_callback, o);
        done = 0;
    }

    if (pa_pstream_is_pending(o->context->pstream)) {
        pa_pstream_set_drain_callback(o->context->pstream, pstream_drain_callback, o);
        done = 0;
    }

    if (!done)
        pa_operation_ref(o);
    else {
        if (o->callback) {
            void (*cb)(struct pa_context *c, void *userdata);
            cb = (void (*)(struct pa_context*, void*)) o->callback;
            cb(o->context, o->userdata);
        }
        
        pa_operation_done(o);
    }   

    pa_operation_unref(o);
}

struct pa_operation* pa_context_drain(struct pa_context *c, void (*cb) (struct pa_context*c, void *userdata), void *userdata) {
    struct pa_operation *o;
    assert(c && c->ref >= 1);

    if (c->state != PA_CONTEXT_READY)
        return NULL;

    if (!pa_context_is_pending(c))
        return NULL;

    o = pa_operation_new(c, NULL);
    assert(o);
    o->callback = cb;
    o->userdata = userdata;

    set_dispatch_callbacks(pa_operation_ref(o));

    return o;
}

void pa_context_exit_daemon(struct pa_context *c) {
    struct pa_tagstruct *t;
    assert(c && c->ref >= 1);
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_EXIT);
    pa_tagstruct_putu32(t, c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
}

void pa_context_simple_ack_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_operation *o = userdata;
    int success = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        success = 0;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        void (*cb)(struct pa_context *c, int success, void *userdata) = o->callback;
        cb(o->context, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

struct pa_operation* pa_context_send_simple_command(struct pa_context *c, uint32_t command, void (*internal_callback)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void (*cb)(), void *userdata) {
    struct pa_tagstruct *t;
    struct pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, command);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, internal_callback, o);

    return pa_operation_ref(o);
}

const char* pa_get_library_version(void) {
    return PACKAGE_VERSION;
}

struct pa_operation* pa_context_set_default_sink(struct pa_context *c, const char *name, void(*cb)(struct pa_context*c, int success, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    struct pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_DEFAULT_SINK);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT,  pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

struct pa_operation* pa_context_set_default_source(struct pa_context *c, const char *name, void(*cb)(struct pa_context*c, int success,  void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    struct pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_DEFAULT_SOURCE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT,  pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

int pa_context_is_local(struct pa_context *c) {
    assert(c);
    return c->local;
}

struct pa_operation* pa_context_set_name(struct pa_context *c, const char *name, void(*cb)(struct pa_context*c, int success,  void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    struct pa_operation *o;
    uint32_t tag;
    assert(c && name && cb);

    o = pa_operation_new(c, NULL);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_CLIENT_NAME);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT,  pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}
