
#ifndef foopolyplibcontexthfoo
#define foopolyplibcontexthfoo

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

#include "sample.h"
#include "polyplib-def.h"
#include "mainloop-api.h"
#include "cdecl.h"
#include "polyplib-operation.h"

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

PA_C_DECL_BEGIN

/** \struct pa_context
 * An opaque connection context to a daemon */
struct pa_context;

/** Instantiate a new connection context with an abstract mainloop API
 * and an application name */
struct pa_context *pa_context_new(struct pa_mainloop_api *mainloop, const char *name);

/** Decrease the reference counter of the context by one */
void pa_context_unref(struct pa_context *c);

/** Increase the reference counter of the context by one */
struct pa_context* pa_context_ref(struct pa_context *c);

/** Set a callback function that is called whenever the context status changes */
void pa_context_set_state_callback(struct pa_context *c, void (*cb)(struct pa_context *c, void *userdata), void *userdata);

/** Return the error number of the last failed operation */
int pa_context_errno(struct pa_context *c);

/** Return non-zero if some data is pending to be written to the connection */
int pa_context_is_pending(struct pa_context *c);

/** Return the current context status */
enum pa_context_state pa_context_get_state(struct pa_context *c);

/** Connect the context to the specified server. If server is NULL,
connect to the default server. This routine may but will not always
return synchronously on error. Use pa_context_set_state_callback() to
be notified when the connection is established */
int pa_context_connect(struct pa_context *c, const char *server);

/** Connect the context to a server. If the default server is local
 * but not accessible, spawn a new daemon. If atfork is not NULL it is
 * run after the fork() in the child process. It may be used to close
 * file descriptors or to do any other cleanups. Make sure that
 * SIGCHLD is handled when calling this function. The function will
 * waitpid() on the daemon's PID. \since 0.4 */
int pa_context_connect_spawn(struct pa_context *c, void (*atfork)(void));

/** Terminate the context connection immediately */
void pa_context_disconnect(struct pa_context *c);

/** Drain the context. If there is nothing to drain, the function returns NULL */
struct pa_operation* pa_context_drain(struct pa_context *c, void (*cb) (struct pa_context*c, void *userdata), void *userdata);

/** Tell the daemon to exit. No operation object is returned as the
 * connection is terminated when the daemon quits, thus this operation
 * would never complete. */
void pa_context_exit_daemon(struct pa_context *c);

/** Set the name of the default sink. \since 0.4 */
struct pa_operation* pa_context_set_default_sink(struct pa_context *c, const char *name, void(*cb)(struct pa_context*c, int success, void *userdata), void *userdata);

/** Set the name of the default source. \since 0.4 */
struct pa_operation* pa_context_set_default_source(struct pa_context *c, const char *name, void(*cb)(struct pa_context*c, int success,  void *userdata), void *userdata);

PA_C_DECL_END

#endif
