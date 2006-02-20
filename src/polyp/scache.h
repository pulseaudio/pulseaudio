#ifndef fooscachehfoo
#define fooscachehfoo

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

#include <sys/types.h>

#include <polyp/context.h>
#include <polyp/stream.h>
#include <polyp/cdecl.h>

/** \file
 * All sample cache related routines */

PA_C_DECL_BEGIN

/** Make this stream a sample upload stream */
void pa_stream_connect_upload(pa_stream *s, size_t length);

/** Finish the sample upload, the stream name will become the sample name. You cancel a samp
 * le upload by issuing pa_stream_disconnect() */
void pa_stream_finish_upload(pa_stream *s);

/** Play a sample from the sample cache to the specified device. If the latter is NULL use the default sink. Returns an operation object */
pa_operation* pa_context_play_sample(
        pa_context *c               /**< Context */,
        const char *name            /**< Name of the sample to play */,
        const char *dev             /**< Sink to play this sample on */,
        pa_volume_t volume          /**< Volume to play this sample with */ ,
        pa_context_success_cb_t cb  /**< Call this function after successfully starting playback, or NULL */,
        void *userdata              /**< Userdata to pass to the callback */);

/** Remove a sample from the sample cache. Returns an operation object which may be used to cancel the operation while it is running */
pa_operation* pa_context_remove_sample(pa_context *c, const char *name, pa_context_success_cb_t, void *userdata);

PA_C_DECL_END

#endif
