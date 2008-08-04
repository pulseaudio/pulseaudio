#ifndef foopulseextstreamrestorehfoo
#define foopulseextstreamrestorehfoo

/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/context.h>

PA_C_DECL_BEGIN

typedef struct pa_ext_stream_restore_info {
    const char *name;
    pa_channel_map channel_map;
    pa_cvolume volume;
    const char *device;
    int mute;
} pa_ext_stream_restore_info;

typedef void (*pa_ext_stream_restore_test_cb_t)(
        pa_context *c,
        uint32_t version,
        void *userdata);

pa_operation *pa_ext_stream_restore_test(
        pa_context *c,
        pa_ext_stream_restore_test_cb_t cb,
        void *userdata);

typedef void (*pa_ext_stream_restore_read_cb_t)(
        pa_context *c,
        const pa_ext_stream_restore_info *info,
        int eol,
        void *userdata);

pa_operation *pa_ext_stream_restore_read(
        pa_context *c,
        pa_ext_stream_restore_read_cb_t cb,
        void *userdata);

pa_operation *pa_ext_stream_restore_write(
        pa_context *c,
        pa_update_mode_t mode,
        const pa_ext_stream_restore_info data[],
        unsigned n,
        int apply_immediately,
        pa_context_success_cb_t cb,
        void *userdata);

pa_operation *pa_ext_stream_restore_delete(
        pa_context *c,
        const char *const s[],
        pa_context_success_cb_t cb,
        void *userdata);

pa_operation *pa_ext_stream_restore_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata);

typedef void (*pa_ext_stream_restore_subscribe_cb_t)(
        pa_context *c,
        void *userdata);

void pa_ext_stream_restore_set_subscribe_cb(
        pa_context *c,
        pa_ext_stream_restore_subscribe_cb_t cb,
        void *userdata);

PA_C_DECL_END

#endif
