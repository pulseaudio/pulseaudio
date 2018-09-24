/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "test-util.h"

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    pa_test_context *ctx = (pa_test_context *) userdata;

    pa_assert(c);
    pa_assert(ctx);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            pa_log_info("Connection established.\n");
            pa_threaded_mainloop_signal(ctx->mainloop, false);
            break;

        case PA_CONTEXT_TERMINATED:
            ctx->mainloop_api->quit(ctx->mainloop_api, 0);
            pa_threaded_mainloop_signal(ctx->mainloop, false);
            break;

        case PA_CONTEXT_FAILED:
            ctx->mainloop_api->quit(ctx->mainloop_api, 0);
            pa_threaded_mainloop_signal(ctx->mainloop, false);
            pa_threaded_mainloop_signal(ctx->mainloop, false);
            pa_log_error("Context error: %s\n", pa_strerror(pa_context_errno(c)));
            pa_assert_not_reached();
            break;

        default:
            pa_assert_not_reached();
    }
}

static void success_cb(pa_context *c, int success, void *userdata) {
    pa_test_context *ctx = (pa_test_context *) userdata;

    pa_assert(c);
    pa_assert(ctx);
    pa_assert(success != 0);

    pa_threaded_mainloop_signal(ctx->mainloop, false);
}

pa_test_context* pa_test_context_new(const char *name) {
    pa_test_context *ctx;
    int r;

    pa_assert(name);

    ctx = pa_xnew0(pa_test_context, 1);

    ctx->modules = pa_idxset_new(NULL, NULL);

    /* Set up a new main loop */
    ctx->mainloop = pa_threaded_mainloop_new();
    pa_assert(ctx->mainloop);

    ctx->mainloop_api = pa_threaded_mainloop_get_api(ctx->mainloop);

    pa_threaded_mainloop_lock(ctx->mainloop);

    pa_threaded_mainloop_start(ctx->mainloop);

    ctx->context = pa_context_new(ctx->mainloop_api, name);
    pa_assert(ctx->context);

    pa_context_set_state_callback(ctx->context, context_state_callback, ctx);

    /* Connect the context */
    r = pa_context_connect(ctx->context, NULL, 0, NULL);
    pa_assert(r == 0);

    pa_threaded_mainloop_wait(ctx->mainloop);

    pa_assert(pa_context_get_state(ctx->context) == PA_CONTEXT_READY);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    return ctx;
}

void pa_test_context_free(pa_test_context *ctx) {
    void *module;
    uint32_t idx;

    pa_threaded_mainloop_lock(ctx->mainloop);

    PA_IDXSET_FOREACH(module, ctx->modules, idx) {
        pa_operation *o;

        o = pa_context_unload_module(ctx->context, PA_PTR_TO_UINT32(module), success_cb, ctx);

        WAIT_FOR_OPERATION(ctx, o);
    }

    pa_context_disconnect(ctx->context);
    pa_context_unref(ctx->context);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    pa_threaded_mainloop_stop(ctx->mainloop);
    pa_threaded_mainloop_free(ctx->mainloop);
}

static void module_index_cb(pa_context *c, uint32_t idx, void *userdata) {
    pa_test_context *ctx = (pa_test_context *) userdata;

    pa_assert(c);
    pa_assert(ctx);
    pa_assert(idx != PA_INVALID_INDEX);

    ctx->module_idx = idx;

    pa_threaded_mainloop_signal(ctx->mainloop, false);
}

static void lookup_module_sink_idx(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    pa_test_context *ctx = (pa_test_context *) userdata;

    pa_assert(ctx);

    if (!i || eol) {
        pa_threaded_mainloop_signal(ctx->mainloop, false);
        return;
    }

    if (i->owner_module == ctx->module_idx)
        ctx->sink_idx = i->index;
}

uint32_t pa_test_context_load_null_sink(pa_test_context *ctx, const char *modargs) {
    pa_operation *o;

    pa_assert(ctx);

    pa_threaded_mainloop_lock(ctx->mainloop);

    /* Load the module */
    ctx->module_idx = PA_INVALID_INDEX;
    o = pa_context_load_module(ctx->context, "module-null-sink", modargs, module_index_cb, ctx);
    WAIT_FOR_OPERATION(ctx, o);

    pa_assert(ctx->module_idx != PA_INVALID_INDEX);
    pa_idxset_put(ctx->modules, PA_UINT32_TO_PTR(ctx->module_idx), NULL);

    /* Look up the sink index corresponding to the module */
    ctx->sink_idx = PA_INVALID_INDEX;
    o = pa_context_get_sink_info_list(ctx->context, lookup_module_sink_idx, ctx);
    WAIT_FOR_OPERATION(ctx, o);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    pa_assert(ctx->sink_idx != PA_INVALID_INDEX);

    return ctx->sink_idx;
}

static void nop_free_cb(void *p) {}

static void underflow_cb(struct pa_stream *s, void *userdata) {
    pa_test_context *ctx = (pa_test_context *) userdata;

    pa_assert(ctx);

    pa_log_info("Stream finished\n");

    pa_threaded_mainloop_signal(ctx->mainloop, false);
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
    pa_test_context *ctx = (pa_test_context *) userdata;

    pa_assert(s);
    pa_assert(ctx);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            break;

        case PA_STREAM_TERMINATED:
            pa_threaded_mainloop_signal(ctx->mainloop, false);
            break;

        case PA_STREAM_READY: {
            int r;

            r = pa_stream_write(s, ctx->data, ctx->length, nop_free_cb, 0, PA_SEEK_ABSOLUTE);
            pa_assert(r == 0);

            /* Be notified when this stream is drained */
            pa_stream_set_underflow_callback(s, underflow_cb, userdata);

            pa_threaded_mainloop_signal(ctx->mainloop, false);
            break;
        }

        case PA_STREAM_FAILED:
            pa_log_error("Stream error: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            pa_threaded_mainloop_signal(ctx->mainloop, false);
            break;

        default:
            pa_assert_not_reached();
    }
}

pa_stream* pa_test_context_create_stream(pa_test_context *ctx, const char *name, uint32_t sink_idx, pa_format_info *format,
                                         pa_stream_flags_t flags, void *data, size_t length) {
    int r;
    pa_stream *s;
    pa_format_info *formats[1];
    char sink_name[5];

    pa_threaded_mainloop_lock(ctx->mainloop);

    formats[0] = format;

    ctx->data = data;
    ctx->length = length;

    s = pa_stream_new_extended(ctx->context, name, formats, 1, NULL);
    pa_assert(s);

    pa_snprintf(sink_name, sizeof(sink_name), "%u", sink_idx);

    pa_stream_set_state_callback(s, stream_state_callback, ctx);
    r = pa_stream_connect_playback(s, sink_name, NULL, flags, NULL, NULL);

    pa_assert(r == 0);

    pa_threaded_mainloop_wait(ctx->mainloop);

    pa_assert(pa_stream_get_state(s) == PA_STREAM_READY);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    return s;
}

void pa_test_context_destroy_stream(pa_test_context *ctx, pa_stream *s) {
    int r;

    pa_threaded_mainloop_lock(ctx->mainloop);

    r = pa_stream_disconnect(s);
    pa_assert(r == 0);

    pa_threaded_mainloop_wait(ctx->mainloop);
    pa_assert(pa_stream_get_state(s) == PA_STREAM_TERMINATED);

    pa_stream_unref(s);

    pa_threaded_mainloop_unlock(ctx->mainloop);
}

struct sink_info_pred {
    pa_test_context *ctx;
    pa_test_sink_info_pred_t func;
    void *userdata;

    bool ret;
};

static void check_sink_info(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    struct sink_info_pred *pred = (struct sink_info_pred *) userdata;

    pa_assert(c);
    pa_assert(pred);

    if (i)
        pred->ret = pred->func(i, pred->userdata);

    pa_threaded_mainloop_signal(pred->ctx->mainloop, false);
}

bool pa_test_context_check_sink(pa_test_context *ctx, uint32_t idx, pa_test_sink_info_pred_t predicate, void *userdata) {
    pa_operation *o;
    struct sink_info_pred pred = { ctx, predicate, userdata, false };

    pa_assert(ctx);
    pa_assert(predicate);

    pa_threaded_mainloop_lock(ctx->mainloop);

    /* Load the module */
    o = pa_context_get_sink_info_by_index(ctx->context, idx, check_sink_info, &pred);
    WAIT_FOR_OPERATION(ctx, o);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    return pred.ret;
}
