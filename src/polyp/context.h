#ifndef foocontexthfoo
#define foocontexthfoo

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

#include <polyp/sample.h>
#include <polyp/def.h>
#include <polyp/mainloop-api.h>
#include <polyp/cdecl.h>
#include <polyp/operation.h>

/** \file
 * Connection contexts for asynchrononous communication with a
 * server. A pa_context object wraps a connection to a polypaudio
 * server using its native protocol. A context may be used to issue
 * commands on the server or to create playback or recording
 * streams. Multiple playback streams may be piped through a single
 * connection context. Operations on the contect involving
 * communication with the server are executed asynchronously: i.e. the
 * client function do not implicitely wait for completion of the
 * operation on the server. Instead the caller specifies a call back
 * function that is called when the operation is completed. Currently
 * running operations may be canceled using pa_operation_cancel(). */

/** \example pacat.c
 * A playback and recording tool using the asynchronous API */

/** \example paplay.c
 * A sound file playback tool using the asynchronous API, based on libsndfile */

PA_C_DECL_BEGIN

/** \struct pa_context
 * An opaque connection context to a daemon */
typedef struct pa_context pa_context;

/** Instantiate a new connection context with an abstract mainloop API
 * and an application name */
pa_context *pa_context_new(pa_mainloop_api *mainloop, const char *name);

/** Decrease the reference counter of the context by one */
void pa_context_unref(pa_context *c);

/** Increase the reference counter of the context by one */
pa_context* pa_context_ref(pa_context *c);

typedef void (*pa_context_state_callback)(pa_context *c, void *userdata);

/** Set a callback function that is called whenever the context status changes */
void pa_context_set_state_callback(pa_context *c, pa_context_state_callback callback, void *userdata);

/** Return the error number of the last failed operation */
int pa_context_errno(pa_context *c);

/** Return non-zero if some data is pending to be written to the connection */
int pa_context_is_pending(pa_context *c);

/** Return the current context status */
pa_context_state_t pa_context_get_state(pa_context *c);

/** Connect the context to the specified server. If server is NULL,
connect to the default server. This routine may but will not always
return synchronously on error. Use pa_context_set_state_callback() to
be notified when the connection is established. If spawn is non-zero
and no specific server is specified or accessible a new daemon is
spawned. If api is non-NULL, the functions specified in the structure
are used when forking a new child process. */
int pa_context_connect(pa_context *c, const char *server, int spawn, const pa_spawn_api *api);

/** Terminate the context connection immediately */
void pa_context_disconnect(pa_context *c);

/** Drain the context. If there is nothing to drain, the function returns NULL */
pa_operation* pa_context_drain(pa_context *c, void (*cb) (pa_context*c, void *userdata), void *userdata);

/** Tell the daemon to exit. No operation object is returned as the
 * connection is terminated when the daemon quits, thus this operation
 * would never complete. */
void pa_context_exit_daemon(pa_context *c);

/** Set the name of the default sink. \since 0.4 */
pa_operation* pa_context_set_default_sink(pa_context *c, const char *name, void(*cb)(pa_context*c, int success, void *userdata), void *userdata);

/** Set the name of the default source. \since 0.4 */
pa_operation* pa_context_set_default_source(pa_context *c, const char *name, void(*cb)(pa_context*c, int success,  void *userdata), void *userdata);

/** Returns 1 when the connection is to a local daemon. Returns negative when no connection has been made yet. \since 0.5 */
int pa_context_is_local(pa_context *c);

/** Set a different application name for context on the server. \since 0.5 */
pa_operation* pa_context_set_name(pa_context *c, const char *name, void(*cb)(pa_context*c, int success,  void *userdata), void *userdata);

/** Return the server name this context is connected to. \since 0.7 */
const char* pa_context_get_server(pa_context *c);

PA_C_DECL_END

#endif
