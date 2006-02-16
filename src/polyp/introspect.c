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

#include <assert.h>

#include "introspect.h"
#include "context.h"
#include "internal.h"
#include <polypcore/pstream-util.h>
#include <polypcore/gccmacro.h>

/*** Statistics ***/

static void context_stat_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    pa_stat_info i, *p = &i;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        p = NULL;
    } else if (pa_tagstruct_getu32(t, &i.memblock_total) < 0 ||
               pa_tagstruct_getu32(t, &i.memblock_total_size) < 0 ||
               pa_tagstruct_getu32(t, &i.memblock_allocated) < 0 ||
               pa_tagstruct_getu32(t, &i.memblock_allocated_size) < 0 ||
               pa_tagstruct_getu32(t, &i.scache_size) < 0 ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        void (*cb)(pa_context *s, const pa_stat_info*_i, void *_userdata) = (void (*)(pa_context *s, const pa_stat_info*_i, void *_userdata)) o->callback;
        cb(o->context, p, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_stat(pa_context *c, void (*cb)(pa_context *c, const pa_stat_info*i, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_STAT, context_stat_callback, (pa_operation_callback) cb, userdata);
}

/*** Server Info ***/

static void context_get_server_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    pa_server_info i, *p = &i;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
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

        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_server_info*_i, void *_userdata) = (void (*)(pa_context *s, const pa_server_info*_i, void *_userdata)) o->callback;
        cb(o->context, p, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_server_info(pa_context *c, void (*cb)(pa_context *c, const pa_server_info*i, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SERVER_INFO, context_get_server_info_callback, (pa_operation_callback) cb, userdata);
}

/*** Sink Info ***/

static void context_get_sink_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_sink_info i;
            
            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_gets(t, &i.description) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_get_cvolume(t, &i.volume) < 0 ||
                pa_tagstruct_getu32(t, &i.monitor_source) < 0 ||
                pa_tagstruct_gets(t, &i.monitor_source_name) < 0 ||
                pa_tagstruct_get_usec(t, &i.latency) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0) {
                
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_sink_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_sink_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_sink_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_sink_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_sink_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sink_info *i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SINK_INFO_LIST, context_get_sink_info_callback, (pa_operation_callback) cb, userdata);
}

pa_operation* pa_context_get_sink_info_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_sink_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SINK_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_sink_info_by_name(pa_context *c, const char *name, void (*cb)(pa_context *c, const pa_sink_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SINK_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_info_callback, o);

    return pa_operation_ref(o);
}

/*** Source info ***/

static void context_get_source_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_source_info i;
            
            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_gets(t, &i.description) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_getu32(t, &i.monitor_of_sink) < 0 ||
                pa_tagstruct_gets(t, &i.monitor_of_sink_name) < 0 ||
                pa_tagstruct_get_usec(t, &i.latency) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0) {
                
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_source_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_source_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_source_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_source_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_source_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_source_info *i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SOURCE_INFO_LIST, context_get_source_info_callback, (pa_operation_callback) cb, userdata);
}

pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_source_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SOURCE_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_source_info_by_name(pa_context *c, const char *name, void (*cb)(pa_context *c, const pa_source_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SOURCE_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_info_callback, o);

    return pa_operation_ref(o);
}

/*** Client info ***/

static void context_get_client_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_client_info i;
            
            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_getu32(t, &i.owner_module) < 0 ||
                pa_tagstruct_gets(t, &i.driver) < 0 ) {
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_client_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_client_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_client_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_client_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_client_info*i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_CLIENT_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_client_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_client_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_client_info*i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_CLIENT_INFO_LIST, context_get_client_info_callback, (pa_operation_callback) cb, userdata);
}

/*** Module info ***/

static void context_get_module_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_module_info i;
            
            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_gets(t, &i.argument) < 0 ||
                pa_tagstruct_getu32(t, &i.n_used) < 0 ||
                pa_tagstruct_get_boolean(t, &i.auto_unload) < 0) {
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_module_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_module_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_module_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_module_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_module_info*i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_MODULE_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_module_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_module_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_module_info*i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_MODULE_INFO_LIST, context_get_module_info_callback, (pa_operation_callback) cb, userdata);
}

/*** Sink input info ***/

static void context_get_sink_input_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_sink_input_info i;
            
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
                pa_tagstruct_gets(t, &i.driver) < 0) {
                
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_sink_input_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_sink_input_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_sink_input_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_sink_input_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_sink_input_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_sink_input_info*i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SINK_INPUT_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sink_input_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_sink_input_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sink_input_info*i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SINK_INPUT_INFO_LIST, context_get_sink_input_info_callback, (pa_operation_callback) cb, userdata);
}

/*** Source output info ***/

static void context_get_source_output_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_source_output_info i;
            
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
                pa_tagstruct_gets(t, &i.driver) < 0) {
                
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_source_output_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_source_output_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_source_output_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_source_output_info*_i, int _eof, void *_userdata))o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_source_output_info*i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SOURCE_OUTPUT_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_source_output_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_source_output_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_source_output_info*i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST, context_get_source_output_info_callback, (pa_operation_callback) cb, userdata);
}

/*** Volume manipulation ***/

pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && idx != PA_INVALID_INDEX);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_SINK_VOLUME);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && name);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_SINK_VOLUME);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && idx != PA_INVALID_INDEX);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SET_SINK_INPUT_VOLUME);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_put_cvolume(t, volume);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

/** Sample Cache **/

static void context_get_sample_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_sample_info i;
            
            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_get_cvolume(t, &i.volume) < 0 ||
                pa_tagstruct_get_usec(t, &i.duration) < 0 ||
                pa_tagstruct_get_sample_spec(t, &i.sample_spec) < 0 ||
                pa_tagstruct_get_channel_map(t, &i.channel_map) < 0 ||
                pa_tagstruct_getu32(t, &i.bytes) < 0 ||
                pa_tagstruct_get_boolean(t, &i.lazy) < 0 ||
                pa_tagstruct_gets(t, &i.filename) < 0) {
                
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_sample_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_sample_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_sample_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_sample_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, void (*cb)(pa_context *c, const pa_sample_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb && name);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SAMPLE_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sample_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_sample_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_SAMPLE_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_tagstruct_puts(t, NULL);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_sample_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_sample_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sample_info *i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_SAMPLE_INFO_LIST, context_get_sample_info_callback, (pa_operation_callback) cb, userdata);
}

static pa_operation* command_kill(pa_context *c, uint32_t command, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && idx != PA_INVALID_INDEX);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, command);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    return command_kill(c, PA_COMMAND_KILL_CLIENT, idx, cb, userdata);
}
                                            
pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    return command_kill(c, PA_COMMAND_KILL_SINK_INPUT, idx, cb, userdata);
}

pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    return command_kill(c, PA_COMMAND_KILL_SOURCE_OUTPUT, idx, cb, userdata);
}

static void load_module_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    uint32_t idx = -1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

    } else if (pa_tagstruct_getu32(t, &idx) < 0 ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    }
    
    if (o->callback) {
        void (*cb)(pa_context *c, uint32_t _idx, void *_userdata) = (void (*)(pa_context *c, uint32_t _idx, void *_userdata)) o->callback;
        cb(o->context, idx, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, void (*cb)(pa_context *c, uint32_t idx, void *userdata), void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && name && argument);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_LOAD_MODULE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_puts(t, argument);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, load_module_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    return command_kill(c, PA_COMMAND_UNLOAD_MODULE, idx, cb, userdata);
}

/*** Autoload stuff ***/

static void context_get_autoload_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int eof = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        eof = -1;
    } else {
        
        while (!pa_tagstruct_eof(t)) {
            pa_autoload_info i;
            
            if (pa_tagstruct_getu32(t, &i.index) < 0 ||
                pa_tagstruct_gets(t, &i.name) < 0 ||
                pa_tagstruct_getu32(t, &i.type) < 0 ||
                pa_tagstruct_gets(t, &i.module) < 0 ||
                pa_tagstruct_gets(t, &i.argument) < 0) {
                pa_context_fail(o->context, PA_ERROR_PROTOCOL);
                goto finish;
            }

            if (o->callback) {
                void (*cb)(pa_context *s, const pa_autoload_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_autoload_info*_i, int _eof, void *_userdata)) o->callback;
                cb(o->context, &i, 0, o->userdata);
            }
        }
    }
    
    if (o->callback) {
        void (*cb)(pa_context *s, const pa_autoload_info*_i, int _eof, void *_userdata) = (void (*)(pa_context *s, const pa_autoload_info*_i, int _eof, void *_userdata)) o->callback;
        cb(o->context, NULL, eof, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_get_autoload_info_by_name(pa_context *c, const char *name, pa_autoload_type_t type, void (*cb)(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb && name);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_AUTOLOAD_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_putu32(t, type);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_autoload_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata), void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    assert(c && cb && idx != PA_INVALID_INDEX);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_AUTOLOAD_INFO);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_get_autoload_info_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_get_autoload_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata), void *userdata) {
    return pa_context_send_simple_command(c, PA_COMMAND_GET_AUTOLOAD_INFO_LIST, context_get_autoload_info_callback, (pa_operation_callback) cb, userdata);
}

static void context_add_autoload_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    uint32_t idx;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        idx = PA_INVALID_INDEX;
    } else if (pa_tagstruct_getu32(t, &idx) ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        void (*cb)(pa_context *s, uint32_t _idx, void *_userdata) = (void (*)(pa_context *s, uint32_t _idx, void *_userdata)) o->callback;
        cb(o->context, idx, o->userdata);
    }


finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type, const char *module, const char*argument, void (*cb)(pa_context *c, int success, void *userdata), void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && name && module && argument);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_ADD_AUTOLOAD);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_putu32(t, type);
    pa_tagstruct_puts(t, module);
    pa_tagstruct_puts(t, argument);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, context_add_autoload_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_remove_autoload_by_name(pa_context *c, const char *name, pa_autoload_type_t type, void (*cb)(pa_context *c, int success, void *userdata), void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && name);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_REMOVE_AUTOLOAD);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_tagstruct_putu32(t, type);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void* userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && idx != PA_INVALID_INDEX);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_REMOVE_AUTOLOAD);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}
