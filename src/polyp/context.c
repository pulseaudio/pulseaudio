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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include "../polypcore/winsock.h"

#include <polyp/version.h>

#include <polypcore/native-common.h>
#include <polypcore/pdispatch.h>
#include <polypcore/pstream.h>
#include <polypcore/dynarray.h>
#include <polypcore/socket-client.h>
#include <polypcore/pstream-util.h>
#include <polypcore/util.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>
#include <polypcore/socket-util.h>

#include "internal.h"

#include "client-conf.h"

#ifdef HAVE_X11
#include "client-conf-x11.h"
#endif

#include "context.h"

#define AUTOSPAWN_LOCK "autospawn.lock"

static const pa_pdispatch_callback command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_REQUEST] = pa_command_request,
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = pa_command_stream_killed,
    [PA_COMMAND_RECORD_STREAM_KILLED] = pa_command_stream_killed,
    [PA_COMMAND_SUBSCRIBE_EVENT] = pa_command_subscribe_event
};

static void unlock_autospawn_lock_file(pa_context *c) {
    assert(c);
    
    if (c->autospawn_lock_fd >= 0) {
        char lf[PATH_MAX];
        pa_runtime_path(AUTOSPAWN_LOCK, lf, sizeof(lf));
        
        pa_unlock_lockfile(lf, c->autospawn_lock_fd);
        c->autospawn_lock_fd = -1;
    }
}

pa_context *pa_context_new(pa_mainloop_api *mainloop, const char *name) {
    pa_context *c;
    assert(mainloop && name);
    
    c = pa_xmalloc(sizeof(pa_context));
    c->ref = 1;
    c->name = pa_xstrdup(name);
    c->mainloop = mainloop;
    c->client = NULL;
    c->pstream = NULL;
    c->pdispatch = NULL;
    c->playback_streams = pa_dynarray_new();
    c->record_streams = pa_dynarray_new();
    assert(c->playback_streams && c->record_streams);

    PA_LLIST_HEAD_INIT(pa_stream, c->streams);
    PA_LLIST_HEAD_INIT(pa_operation, c->operations);
    
    c->error = PA_ERROR_OK;
    c->state = PA_CONTEXT_UNCONNECTED;
    c->ctag = 0;

    c->state_callback = NULL;
    c->state_userdata = NULL;

    c->subscribe_callback = NULL;
    c->subscribe_userdata = NULL;

    c->memblock_stat = pa_memblock_stat_new();
    c->local = -1;
    c->server_list = NULL;
    c->server = NULL;
    c->autospawn_lock_fd = -1;
    memset(&c->spawn_api, 0, sizeof(c->spawn_api));
    c->do_autospawn = 0;

#ifdef SIGPIPE    
    pa_check_signal_is_blocked(SIGPIPE);
#endif

    c->conf = pa_client_conf_new();
    pa_client_conf_load(c->conf, NULL);
#ifdef HAVE_X11
    pa_client_conf_from_x11(c->conf, NULL);
#endif
    pa_client_conf_env(c->conf);
    
    return c;
}

static void context_free(pa_context *c) {
    assert(c);

    unlock_autospawn_lock_file(c);

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

    pa_strlist_free(c->server_list);
    
    pa_xfree(c->name);
    pa_xfree(c->server);
    pa_xfree(c);
}

pa_context* pa_context_ref(pa_context *c) {
    assert(c && c->ref >= 1);
    c->ref++;
    return c;
}

void pa_context_unref(pa_context *c) {
    assert(c && c->ref >= 1);

    if ((--(c->ref)) == 0)
        context_free(c);
}

void pa_context_set_state(pa_context *c, pa_context_state_t st) {
    assert(c);
    
    if (c->state == st)
        return;

    pa_context_ref(c);

    if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
        pa_stream *s;
        
        s = c->streams ? pa_stream_ref(c->streams) : NULL;
        while (s) {
            pa_stream *n = s->next ? pa_stream_ref(s->next) : NULL;
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

void pa_context_fail(pa_context *c, int error) {
    assert(c);
    c->error = error;
    pa_context_set_state(c, PA_CONTEXT_FAILED);
}

static void pstream_die_callback(pa_pstream *p, void *userdata) {
    pa_context *c = userdata;
    assert(p && c);
    pa_context_fail(c, PA_ERROR_CONNECTIONTERMINATED);
}

static void pstream_packet_callback(pa_pstream *p, pa_packet *packet, void *userdata) {
    pa_context *c = userdata;
    assert(p && packet && c);

    pa_context_ref(c);
    
    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        pa_log(__FILE__": invalid packet.\n");
        pa_context_fail(c, PA_ERROR_PROTOCOL);
    }

    pa_context_unref(c);
}

static void pstream_memblock_callback(pa_pstream *p, uint32_t channel, PA_GCC_UNUSED uint32_t delta, const pa_memchunk *chunk, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    assert(p && chunk && c && chunk->memblock && chunk->memblock->data);

    pa_context_ref(c);
    
    if ((s = pa_dynarray_get(c->record_streams, channel))) {
        pa_mcalign_push(s->mcalign, chunk);

        for (;;) {
            pa_memchunk t;

            if (pa_mcalign_pop(s->mcalign, &t) < 0)
                break;

            assert(s->record_memblockq);
            pa_memblockq_push(s->record_memblockq, &t, t.length);
            if (s->read_callback)
                s->read_callback(s, pa_stream_readable_size(s), s->read_userdata);

            pa_memblock_unref(t.memblock);
        }
    }

    pa_context_unref(c);
}

int pa_context_handle_error(pa_context *c, uint32_t command, pa_tagstruct *t) {
    assert(c);

    if (command == PA_COMMAND_ERROR) {
        assert(t);
        
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

static void setup_complete_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
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
            pa_tagstruct *reply;
            reply = pa_tagstruct_new(NULL, 0);
            pa_tagstruct_putu32(reply, PA_COMMAND_SET_CLIENT_NAME);
            pa_tagstruct_putu32(reply, tag = c->ctag++);
            pa_tagstruct_puts(reply, c->name);
            pa_pstream_send_tagstruct(c->pstream, reply);
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

static void setup_context(pa_context *c, pa_iochannel *io) {
    pa_tagstruct *t;
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

    if (!c->conf->cookie_valid) {
        pa_context_fail(c, PA_ERROR_AUTHKEY);
        goto finish;
    }

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_AUTH);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_put_arbitrary(t, c->conf->cookie, sizeof(c->conf->cookie));
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, setup_complete_callback, c);

    pa_context_set_state(c, PA_CONTEXT_AUTHORIZING);

finish:
    
    pa_context_unref(c);
}

static void on_connection(pa_socket_client *client, pa_iochannel*io, void *userdata);

#ifndef OS_IS_WIN32

static int context_connect_spawn(pa_context *c) {
    pid_t pid;
    int status, r;
    int fds[2] = { -1, -1} ;
    pa_iochannel *io;

    pa_context_ref(c);
    
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        pa_log(__FILE__": socketpair() failed: %s\n", strerror(errno));
        pa_context_fail(c, PA_ERROR_INTERNAL);
        goto fail;
    }

    pa_fd_set_cloexec(fds[0], 1);
    
    pa_socket_low_delay(fds[0]);
    pa_socket_low_delay(fds[1]);

    if (c->spawn_api.prefork)
        c->spawn_api.prefork();

    if ((pid = fork()) < 0) {
        pa_log(__FILE__": fork() failed: %s\n", strerror(errno));
        pa_context_fail(c, PA_ERROR_INTERNAL);

        if (c->spawn_api.postfork)
            c->spawn_api.postfork();
        
        goto fail;
    } else if (!pid) {
        /* Child */

        char t[128];
        const char *state = NULL;
#define MAX_ARGS 64
        const char * argv[MAX_ARGS+1];
        int n;

        /* Not required, since fds[0] has CLOEXEC enabled anyway */
        close(fds[0]);
        
        if (c->spawn_api.atfork)
            c->spawn_api.atfork();

        /* Setup argv */

        n = 0;
        
        argv[n++] = c->conf->daemon_binary;
        argv[n++] = "--daemonize=yes";
        
        snprintf(t, sizeof(t), "-Lmodule-native-protocol-fd fd=%i", fds[1]);
        argv[n++] = strdup(t);

        while (n < MAX_ARGS) {
            char *a;

            if (!(a = pa_split_spaces(c->conf->extra_arguments, &state)))
                break;
            
            argv[n++] = a;
        }

        argv[n++] = NULL;

        execv(argv[0], (char * const *) argv);
        _exit(1);
#undef MAX_ARGS
    } 

    /* Parent */

    r = waitpid(pid, &status, 0);

    if (c->spawn_api.postfork)
        c->spawn_api.postfork();
        
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
    unlock_autospawn_lock_file(c);

    pa_context_unref(c);

    return 0;

fail:
    if (fds[0] != -1)
        close(fds[0]);
    if (fds[1] != -1)
        close(fds[1]);

    unlock_autospawn_lock_file(c);

    pa_context_unref(c);

    return -1;
}

#endif /* OS_IS_WIN32 */

static int try_next_connection(pa_context *c) {
    char *u = NULL;
    int r = -1;
    assert(c && !c->client);

    for (;;) {
        if (u)
            pa_xfree(u);
        u = NULL;
        
        c->server_list = pa_strlist_pop(c->server_list, &u);
        
        if (!u) {

#ifndef OS_IS_WIN32
            if (c->do_autospawn) {
                r = context_connect_spawn(c);
                goto finish;
            }
#endif
            
            pa_context_fail(c, PA_ERROR_CONNECTIONREFUSED);
            goto finish;
        }
        
        pa_log_debug(__FILE__": Trying to connect to %s...\n", u);  

        pa_xfree(c->server);
        c->server = pa_xstrdup(u);

        if (!(c->client = pa_socket_client_new_string(c->mainloop, u, PA_NATIVE_DEFAULT_PORT)))
            continue;
        
        c->local = pa_socket_client_is_local(c->client);
        pa_socket_client_set_callback(c->client, on_connection, c);
        break;
    }

    r = 0;

finish:
    if (u)
        pa_xfree(u);
    
    return r;
}

static void on_connection(pa_socket_client *client, pa_iochannel*io, void *userdata) {
    pa_context *c = userdata;
    assert(client && c && c->state == PA_CONTEXT_CONNECTING);

    pa_context_ref(c);

    pa_socket_client_unref(client);
    c->client = NULL;

    if (!io) {
        /* Try the item in the list */
        if (errno == ECONNREFUSED || errno == ETIMEDOUT || errno == EHOSTUNREACH) {
            try_next_connection(c);
            goto finish;
        }

        pa_context_fail(c, PA_ERROR_CONNECTIONREFUSED);
        goto finish;
    }

    unlock_autospawn_lock_file(c);
    setup_context(c, io);

finish:
    pa_context_unref(c);
}

int pa_context_connect(pa_context *c, const char *server, int spawn, const pa_spawn_api *api) {
    int r = -1;
    assert(c && c->ref >= 1 && c->state == PA_CONTEXT_UNCONNECTED);

    if (!server)
        server = c->conf->default_server;

    pa_context_ref(c);

    assert(!c->server_list);
    
    if (server) {
        if (!(c->server_list = pa_strlist_parse(server))) {
            pa_context_fail(c, PA_ERROR_INVALIDSERVER);
            goto finish;
        }
    } else {
        char *d;
        char ufn[PATH_MAX];

        /* Prepend in reverse order */
        
        if ((d = getenv("DISPLAY"))) {
            char *e;
            d = pa_xstrdup(d);
            if ((e = strchr(d, ':')))
                *e = 0;

            if (*d)
                c->server_list = pa_strlist_prepend(c->server_list, d);

            pa_xfree(d);
        }
        
        c->server_list = pa_strlist_prepend(c->server_list, "tcp6:localhost");
        c->server_list = pa_strlist_prepend(c->server_list, "localhost");
        c->server_list = pa_strlist_prepend(c->server_list, pa_runtime_path(PA_NATIVE_DEFAULT_UNIX_SOCKET, ufn, sizeof(ufn)));

        /* Wrap the connection attempts in a single transaction for sane autospawn locking */
        if (spawn && c->conf->autospawn) {
            char lf[PATH_MAX];

            pa_runtime_path(AUTOSPAWN_LOCK, lf, sizeof(lf));
            pa_make_secure_parent_dir(lf);
            assert(c->autospawn_lock_fd <= 0);
            c->autospawn_lock_fd = pa_lock_lockfile(lf);

            if (api)
                c->spawn_api = *api;
            c->do_autospawn = 1;
        }

    }

    pa_context_set_state(c, PA_CONTEXT_CONNECTING);
    r = try_next_connection(c);
    
finish:
    pa_context_unref(c);
    
    return r;
}

void pa_context_disconnect(pa_context *c) {
    assert(c);
    pa_context_set_state(c, PA_CONTEXT_TERMINATED);
}

pa_context_state_t pa_context_get_state(pa_context *c) {
    assert(c && c->ref >= 1);
    return c->state;
}

int pa_context_errno(pa_context *c) {
    assert(c && c->ref >= 1);
    return c->error;
}

void pa_context_set_state_callback(pa_context *c, void (*cb)(pa_context *c, void *userdata), void *userdata) {
    assert(c && c->ref >= 1);
    c->state_callback = cb;
    c->state_userdata = userdata;
}

int pa_context_is_pending(pa_context *c) {
    assert(c && c->ref >= 1);

/*     pa_log("pstream: %i\n", pa_pstream_is_pending(c->pstream)); */
/*     pa_log("pdispatch: %i\n", pa_pdispatch_is_pending(c->pdispatch)); */
    
    return (c->pstream && pa_pstream_is_pending(c->pstream)) ||
        (c->pdispatch && pa_pdispatch_is_pending(c->pdispatch)) ||
        c->client;
}

static void set_dispatch_callbacks(pa_operation *o);

static void pdispatch_drain_callback(PA_GCC_UNUSED pa_pdispatch*pd, void *userdata) {
    set_dispatch_callbacks(userdata);
}

static void pstream_drain_callback(PA_GCC_UNUSED pa_pstream *s, void *userdata) {
    set_dispatch_callbacks(userdata);
}

static void set_dispatch_callbacks(pa_operation *o) {
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
            void (*cb)(pa_context *c, void *userdata);
            cb = (void (*)(pa_context*, void*)) o->callback;
            cb(o->context, o->userdata);
        }
        
        pa_operation_done(o);
    }   

    pa_operation_unref(o);
}

pa_operation* pa_context_drain(pa_context *c, void (*cb) (pa_context*c, void *userdata), void *userdata) {
    pa_operation *o;
    assert(c && c->ref >= 1);

    if (c->state != PA_CONTEXT_READY)
        return NULL;

    if (!pa_context_is_pending(c))
        return NULL;

    o = pa_operation_new(c, NULL);
    assert(o);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    set_dispatch_callbacks(pa_operation_ref(o));

    return o;
}

void pa_context_exit_daemon(pa_context *c) {
    pa_tagstruct *t;
    assert(c && c->ref >= 1);
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_EXIT);
    pa_tagstruct_putu32(t, c->ctag++);
    pa_pstream_send_tagstruct(c->pstream, t);
}

void pa_context_simple_ack_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
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
        void (*cb)(pa_context *c, int _success, void *_userdata) = (void (*)(pa_context *c, int _success, void *_userdata)) o->callback;
        cb(o->context, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_send_simple_command(pa_context *c, uint32_t command, void (*internal_callback)(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata), void (*cb)(void), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
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

pa_operation* pa_context_set_default_sink(pa_context *c, const char *name, void(*cb)(pa_context*c, int success, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_DEFAULT_SINK);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT,  pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_set_default_source(pa_context *c, const char *name, void(*cb)(pa_context*c, int success,  void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_DEFAULT_SOURCE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT,  pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

int pa_context_is_local(pa_context *c) {
    assert(c);
    return c->local;
}

pa_operation* pa_context_set_name(pa_context *c, const char *name, void(*cb)(pa_context*c, int success,  void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && name && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_CLIENT_NAME);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT,  pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

const char* pa_get_library_version(void) {
    return PACKAGE_VERSION;
}

const char* pa_context_get_server(pa_context *c) {

    if (!c->server)
        return NULL;
    
    if (*c->server == '{') {
        char *e = strchr(c->server+1, '}');
        return e ? e+1 : c->server;
    }
    
    return c->server;
}
