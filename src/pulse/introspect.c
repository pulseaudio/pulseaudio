/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <pulse/context.h>
#include <pulse/gccmacro.h>

#include <pulsecore/macro.h>
#include <pulsecore/pstream-util.h>

#include "internal.h"

#include "introspect.h"

/*** Statistics ***/

static void context_stat_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    pa_stat_info i, *p = &i;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    memset(&i, 0, sizeof(i));

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        p = NULL;
    } else if (pa_tagstruct_getu32(t, &i.memblock_total) < 0 ||
               pa_tagstruct_getu32(t, &i.memblock_total_size) < 0 ||
               pa_tagstruct_getu32(t, &i.memblock_allocated) < 0 ||
               pa_tagstruct_getu32(t, &i.memblock_allocated_size) < 0 ||
               pa_tagstruct_getu32(t, &i.scache_size) < 0) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        pa_stat_info_cb_t cb = (pa_stat_info_cb_t) o->callback;
        cb(o->context, p, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_stat(pa_context *c, pa_stat_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_STAT, context_stat_callback, (pa_operation_cb_t) cb, userdata);
}

/*** Server Info ***/

static void context_get_server_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    pa_server_info i, *p = &i;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    memset(&i, 0, sizeof(i));

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        p = NULL;
    } else if (pa_tagstruct_gets(t, &i.server_name) < 0 ||
               pa_tagstruct_gets(t, &i.server_version) < 0 ||
               pa_tagstruct_gets(t, &i.user_name) < 0 ||
               pa_tagstruct_gets(t, &i.host_name) < 0 ||
               pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
               pa_tagstruct_gets(t, &i.default_sink_name) < 0 ||
               pa_tagstruct_gets(t, &i.default_source_name) < 0 ||
               pa_tagstruct_getu32(t, &i.cookie) < 0 ||
               !pa_tagstruct_eof(t)) {

        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        pa_server_info_cb_t cb = (pa_server_info_cb_t) o->callback;
        cb(o->context, p, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SERVER_INFO, context_get_server_info_callback, (pa_operation_cb_t) cb, userdata);
}

/*** Sink Info ***/

static void context_get_sink_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {
        uint32_t flags;

        while (!pa_tagstruct_eof(t)) {
            pa_sink_info i;
            pa_bool_t mute = FALSE;

            memset(&i, 0, sizeof(i));
            i.proplist = pa_proplist_new();

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_gets(t, &i.description) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_get_cvolume(t, &i.volume) < 0 ||
                pa_tagstruct_get_boolean(t, &mute) < 0 ||
                pa_tagstruct_getu32(t, &i.monitor_source) < 0 ||
                pa_tagstruct_gets(t, &i.monitor_source_name) < 0 ||
                pa_tagstruct_get_usec(t, &i.latency) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0 ||
                pa_tagstruct_getu32(t, &flags) < 0 ||
                (o->context->version >= 13 &&
                 (pa_tagstruct_get_proplist(t, i.proplist) < 0 ||
                  pa_tagstruct_get_usec(t, &i.configured_latency) < 0))) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                pa_proplist_free(i.proplist);
                goto finish;
            }

            i.mute = (int) mute;
            i.flags = (pa_sink_flags_t) flags;

            if (o->callback) {
                pa_sink_info_cb_t cb = (pa_sink_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }

            pa_proplist_free(i.proplist);
        }
    }

    if (o->callback) {
        pa_sink_info_cb_t cb = (pa_sink_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SINK_INFO_LIST, context_get_sink_info_callback, (pa_operation_cb_t) cb, userdata);
}

pa_operation* pa_context_get_sink_info_by_index(pa_context *c, uint32_t idx, pa_sink_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SINK_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_sink_info_by_name(pa_context *c, const char *name, pa_sink_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SINK_INFO, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

/*** Source info ***/

static void context_get_source_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_source_info i;
            uint32_t flags;
            pa_bool_t mute = FALSE;

            memset(&i, 0, sizeof(i));
            i.proplist = pa_proplist_new();

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_gets(t, &i.description) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_get_cvolume(t, &i.volume) < 0 ||
                pa_tagstruct_get_boolean(t, &mute) < 0 ||
                pa_tagstruct_getu32(t, &i.monitor_of_sink) < 0 ||
                pa_tagstruct_gets(t, &i.monitor_of_sink_name) < 0 ||
                pa_tagstruct_get_usec(t, &i.latency) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0 ||
                pa_tagstruct_getu32(t, &flags) < 0 ||
                (o->context->version >= 13 &&
                 (pa_tagstruct_get_proplist(t, i.proplist) < 0 ||
                  pa_tagstruct_get_usec(t, &i.configured_latency) < 0))) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                pa_proplist_free(i.proplist);
                goto finish;
            }

            i.mute = (int) mute;
            i.flags = (pa_source_flags_t) flags;

            if (o->callback) {
                pa_source_info_cb_t cb = (pa_source_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }

            pa_proplist_free(i.proplist);
        }
    }

    if (o->callback) {
        pa_source_info_cb_t cb = (pa_source_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SOURCE_INFO_LIST, context_get_source_info_callback, (pa_operation_cb_t) cb, userdata);
}

pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t idx, pa_source_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SOURCE_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_source_info_by_name(pa_context *c, const char *name, pa_source_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SOURCE_INFO, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

/*** Client info ***/

static void context_get_client_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_client_info i;

            memset(&i, 0, sizeof(i));
            i.proplist = pa_proplist_new();

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0 ||
                (o->context->version >= 13 && pa_tagstruct_get_proplist(t, i.proplist) < 0)) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                pa_proplist_free(i.proplist);
                goto finish;
            }

            if (o->callback) {
                pa_client_info_cb_t cb = (pa_client_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }

            pa_proplist_free(i.proplist);
        }
    }

    if (o->callback) {
        pa_client_info_cb_t cb = (pa_client_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, pa_client_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_CLIENT_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_client_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_client_info_list(pa_context *c, pa_client_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_CLIENT_INFO_LIST, context_get_client_info_callback, (pa_operation_cb_t) cb, userdata);
}

/*** Module info ***/

static void context_get_module_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_module_info i;
            pa_bool_t auto_unload = FALSE;
            memset(&i, 0, sizeof(i));

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_gets(t, &i.argument) < 0 ||
                pa_tagstruct_getu32(t, &i.n_used) < 0 ||
                pa_tagstruct_get_boolean(t, &auto_unload) < 0) {
                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }

            i.auto_unload = (int) auto_unload;

            if (o->callback) {
                pa_module_info_cb_t cb = (pa_module_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }

    if (o->callback) {
        pa_module_info_cb_t cb = (pa_module_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, pa_module_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_MODULE_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_module_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_module_info_list(pa_context *c, pa_module_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_MODULE_INFO_LIST, context_get_module_info_callback, (pa_operation_cb_t) cb, userdata);
}

/*** Sink input info ***/

static void context_get_sink_input_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_sink_input_info i;
            pa_bool_t mute = FALSE;

            memset(&i, 0, sizeof(i));
            i.proplist = pa_proplist_new();

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_getu32(t, &i.client) < 0 ||
                pa_tagstruct_getu32(t, &i.sink) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_get_cvolume(t, &i.volume) < 0 ||
                pa_tagstruct_get_usec(t, &i.buffer_usec) < 0 ||
                pa_tagstruct_get_usec(t, &i.sink_usec) < 0 ||
                pa_tagstruct_gets(t, &i.resample_method) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0 ||
                (o->context->version >= 11 && pa_tagstruct_get_boolean(t, &mute) < 0) ||
                (o->context->version >= 13 && pa_tagstruct_get_proplist(t, i.proplist) < 0)) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                pa_proplist_free(i.proplist);
                goto finish;
            }

            i.mute = (int) mute;

            if (o->callback) {
                pa_sink_input_info_cb_t cb = (pa_sink_input_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }

            pa_proplist_free(i.proplist);
        }
    }

    if (o->callback) {
        pa_sink_input_info_cb_t cb = (pa_sink_input_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SINK_INPUT_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_input_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_sink_input_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sink_input_info*i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SINK_INPUT_INFO_LIST, context_get_sink_input_info_callback, (pa_operation_cb_t) cb, userdata);
}

/*** Source output info ***/

static void context_get_source_output_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_source_output_info i;

            memset(&i, 0, sizeof(i));
            i.proplist = pa_proplist_new();

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_getu32(t, &i.client) < 0 ||
                pa_tagstruct_getu32(t, &i.source) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_get_usec(t, &i.buffer_usec) < 0 ||
                pa_tagstruct_get_usec(t, &i.source_usec) < 0 ||
                pa_tagstruct_gets(t, &i.resample_method) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0 ||
                (o->context->version >= 13 && pa_tagstruct_get_proplist(t, i.proplist) < 0)) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                pa_proplist_free(i.proplist);
                goto finish;
            }

            if (o->callback) {
                pa_source_output_info_cb_t cb = (pa_source_output_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }

            pa_proplist_free(i.proplist);
        }
    }

    if (o->callback) {
        pa_source_output_info_cb_t cb = (pa_source_output_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, pa_source_output_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SOURCE_OUTPUT_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_output_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_source_output_info_list(pa_context *c,  pa_source_output_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST, context_get_source_output_info_callback, (pa_operation_cb_t) cb, userdata);
}

/*** Volume manipulation ***/

pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(volume);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SINK_VOLUME, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(name);
    pa_assert(volume);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SINK_VOLUME, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_sink_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SINK_MUTE, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_tagstruct_put_boolean(t, mute);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_sink_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(name);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SINK_MUTE, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_boolean(t, mute);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(volume);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SINK_INPUT_VOLUME, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 11, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SINK_INPUT_MUTE, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_put_boolean(t, mute);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_source_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(volume);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SOURCE_VOLUME, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_source_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(name);
    pa_assert(volume);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SOURCE_VOLUME, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_source_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SOURCE_MUTE, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_tagstruct_put_boolean(t, mute);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_set_source_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(name);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SET_SOURCE_MUTE, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_boolean(t, mute);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

/** Sample Cache **/

static void context_get_sample_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_sample_info i;
            pa_bool_t lazy = FALSE;

            memset(&i, 0, sizeof(i));
            i.proplist = pa_proplist_new();

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_get_cvolume(t, &i.volume) < 0 ||
                pa_tagstruct_get_usec(t, &i.duration) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_getu32(t, &i.bytes) < 0 ||
                pa_tagstruct_get_boolean(t, &lazy) < 0 ||
                pa_tagstruct_gets(t, &i.filename) < 0 ||
                (o->context->version >= 13 && pa_tagstruct_get_proplist(t, i.proplist) < 0)) {

                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }

            i.lazy = (int) lazy;

            if (o->callback) {
                pa_sample_info_cb_t cb = (pa_sample_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }

            pa_proplist_free(i.proplist);
        }
    }

    if (o->callback) {
        pa_sample_info_cb_t cb = (pa_sample_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, pa_sample_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SAMPLE_INFO, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sample_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, pa_sample_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_SAMPLE_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sample_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_get_sample_info_list(pa_context *c, pa_sample_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SAMPLE_INFO_LIST, context_get_sample_info_callback, (pa_operation_cb_t) cb, userdata);
}

static pa_operation* command_kill(pa_context *c, uint32_t command, uint32_t idx, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, command, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata) {
    return command_kill(c, PA_COMMAND_KILL_CLIENT, idx, cb, userdata);
}

pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata) {
    return command_kill(c, PA_COMMAND_KILL_SINK_INPUT, idx, cb, userdata);
}

pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata) {
    return command_kill(c, PA_COMMAND_KILL_SOURCE_OUTPUT, idx, cb, userdata);
}

static void context_index_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    uint32_t idx;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        idx = PA_INVALID_INDEX;
    } else if (pa_tagstruct_getu32(t, &idx) ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        pa_context_index_cb_t cb = (pa_context_index_cb_t) o->callback;
        cb(o->context, idx, o->userdata);
    }


finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, pa_context_index_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_LOAD_MODULE, &tag);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_puts(t, argument);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_index_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata) {
    return command_kill(c, PA_COMMAND_UNLOAD_MODULE, idx, cb, userdata);
}

/*** Autoload stuff ***/

static void context_get_autoload_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eol = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t, FALSE) < 0)
            goto finish;

        eol = -1;
    } else {

        while (!pa_tagstruct_eof(t)) {
            pa_autoload_info i;

            memset(&i, 0, sizeof(i));

            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_getu32(t, &i.type) < 0 ||
                pa_tagstruct_gets(t, &i.module) < 0 ||
                pa_tagstruct_gets(t, &i.argument) < 0) {
                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                pa_autoload_info_cb_t cb = (pa_autoload_info_cb_t) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }

    if (o->callback) {
        pa_autoload_info_cb_t cb = (pa_autoload_info_cb_t) o->callback;
        cb(o->context, NULL, eol, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

PA_WARN_REFERENCE(pa_context_get_autoload_info_by_name, "Autoload will no longer be implemented by future versions of the PulseAudio server.");

pa_operation* pa_context_get_autoload_info_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_autoload_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, type == PA_AUTOLOAD_SINK || type == PA_AUTOLOAD_SOURCE, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_AUTOLOAD_INFO, &tag);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_putu32(t, type);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_autoload_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

PA_WARN_REFERENCE(pa_context_get_autoload_info_by_index, "Autoload will no longer be implemented by future versions of the PulseAudio server.");

pa_operation* pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx, pa_autoload_info_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);
    pa_assert(cb);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_GET_AUTOLOAD_INFO, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_autoload_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}


PA_WARN_REFERENCE(pa_context_get_autoload_info_list, "Autoload will no longer be implemented by future versions of the PulseAudio server.");

pa_operation* pa_context_get_autoload_info_list(pa_context *c, pa_autoload_info_cb_t cb, void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_AUTOLOAD_INFO_LIST, context_get_autoload_info_callback, (pa_operation_cb_t) cb, userdata);
}

PA_WARN_REFERENCE(pa_context_add_autoload, "Autoload will no longer be implemented by future versions of the PulseAudio server.");

pa_operation* pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type, const char *module, const char*argument, pa_context_index_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, type == PA_AUTOLOAD_SINK || type == PA_AUTOLOAD_SOURCE, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, module && *module, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_ADD_AUTOLOAD, &tag);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_putu32(t, type);
    pa_tagstruct_puts(t, module);
    pa_tagstruct_puts(t, argument);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_index_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

PA_WARN_REFERENCE(pa_context_remove_autoload_by_name, "Autoload will no longer be implemented by future versions of the PulseAudio server.");

pa_operation* pa_context_remove_autoload_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, type == PA_AUTOLOAD_SINK || type == PA_AUTOLOAD_SOURCE, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_REMOVE_AUTOLOAD, &tag);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_putu32(t, type);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

PA_WARN_REFERENCE(pa_context_remove_autoload_by_index, "Autoload will no longer be implemented by future versions of the PulseAudio server.");

pa_operation* pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_REMOVE_AUTOLOAD, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 10, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, sink_name && *sink_name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_MOVE_SINK_INPUT, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, sink_name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_move_sink_input_by_index(pa_context *c, uint32_t idx, uint32_t sink_idx, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 10, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, sink_idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_MOVE_SINK_INPUT, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_putu32(t, sink_idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_move_source_output_by_name(pa_context *c, uint32_t idx, const char *source_name, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 10, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, source_name && *source_name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_MOVE_SOURCE_OUTPUT, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, source_name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_move_source_output_by_index(pa_context *c, uint32_t idx, uint32_t source_idx, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 10, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, source_idx != PA_INVALID_INDEX, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_MOVE_SOURCE_OUTPUT, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_putu32(t, source_idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_suspend_sink_by_name(pa_context *c, const char *sink_name, int suspend, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 11, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !sink_name || *sink_name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SUSPEND_SINK, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, sink_name);
    pa_tagstruct_put_boolean(t, suspend);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_suspend_sink_by_index(pa_context *c, uint32_t idx, int suspend, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 11, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SUSPEND_SINK, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, idx == PA_INVALID_INDEX ? "" : NULL);
    pa_tagstruct_put_boolean(t, suspend);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_suspend_source_by_name(pa_context *c, const char *source_name, int suspend, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 11, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !source_name || *source_name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SUSPEND_SOURCE, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, source_name);
    pa_tagstruct_put_boolean(t, suspend);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_suspend_source_by_index(pa_context *c, uint32_t idx, int suspend, pa_context_success_cb_t cb, void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 11, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_SUSPEND_SOURCE, &tag);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, idx == PA_INVALID_INDEX ? "" : NULL);
    pa_tagstruct_put_boolean(t, suspend);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}
