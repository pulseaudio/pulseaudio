#ifndef foopolyplibscachehfoo
#define foopolyplibscachehfoo

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

#include <sys/types.h>

#include "polyplib-context.h"
#include "polyplib-stream.h"
#include "cdecl.h"

/** \file
 * All sample cache related routines */

PA_C_DECL_BEGIN

/** Make this stream a sample upload stream */
void pa_stream_connect_upload(struct pa_stream *s, size_t length);

/** Finish the sample upload, the stream name will become the sample name. You cancel a sample upload by issuing pa_stream_disconnect() */
void pa_stream_finish_upload(struct pa_stream *s);

/** Play a sample from the sample cache to the specified device. If the latter is NULL use the default sink. Returns an operation object */
struct pa_operation* pa_context_play_sample(struct pa_context *c, const char *name, const char *dev, uint32_t volume, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);

/** Remove a sample from the sample cache. Returns an operation object which may be used to cancel the operation while it is running */
struct pa_operation* pa_context_remove_sample(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);

PA_C_DECL_END

#endif
