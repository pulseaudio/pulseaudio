/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

/* TODO: Some plugins cause latency, and some even report it by using a control
   out port. We don't currently use the latency information. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>

#include "module-ladspa-sink-symdef.h"
#include "ladspa.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Virtual LADSPA sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "master=<name of sink to remap> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map> "
        "plugin=<ladspa plugin name> "
        "label=<ladspa plugin label> "
        "control=<comma seperated list of input control values>")

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_sink *sink, *master;
    pa_sink_input *sink_input;

    const LADSPA_Descriptor *descriptor;
    unsigned channels;
    LADSPA_Handle handle[PA_CHANNELS_MAX];
    LADSPA_Data *input, *output;
    size_t block_size;
    unsigned long input_port, output_port;
    LADSPA_Data *control;

    /* This is a dummy buffer. Every port must be connected, but we don't care
       about control out ports. We connect them all to this single buffer. */
    LADSPA_Data control_out;

    pa_memchunk memchunk;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "master",
    "format",
    "channels",
    "rate",
    "channel_map",
    "plugin",
    "label",
    "control",
    NULL
};

/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;

            if (PA_MSGOBJECT(u->master)->process_msg(PA_MSGOBJECT(u->master), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            *((pa_usec_t*) data) = usec + pa_bytes_to_usec(u->memchunk.length, &u->sink->sample_spec);
            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (PA_SINK_LINKED(state) && u->sink_input && PA_SINK_INPUT_LINKED(pa_sink_input_get_state(u->sink_input)))
        pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);

    return 0;
}

/* Called from I/O thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK_INPUT(o)->userdata;

    switch (code) {
        case PA_SINK_INPUT_MESSAGE_GET_LATENCY:
            *((pa_usec_t*) data) = pa_bytes_to_usec(u->memchunk.length, &u->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
    }

    return pa_sink_input_process_msg(o, code, data, offset, chunk);
}

/* Called from I/O thread context */
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->memchunk.memblock) {
        pa_memchunk tchunk;
        float *src, *dst;
        size_t fs;
        unsigned n, c;

        pa_sink_render(u->sink, length, &tchunk);

        fs = pa_frame_size(&i->sample_spec);
        n = tchunk.length / fs;

        pa_assert(n > 0);

        u->memchunk.memblock = pa_memblock_new(i->sink->core->mempool, tchunk.length);
        u->memchunk.index = 0;
        u->memchunk.length = tchunk.length;

        src = (float*) ((uint8_t*) pa_memblock_acquire(tchunk.memblock) + tchunk.index);
        dst = (float*) pa_memblock_acquire(u->memchunk.memblock);

        for (c = 0; c < u->channels; c++) {
            unsigned j;
            float *p, *q;

            p = src + c;
            q = u->input;
            for (j = 0; j < n; j++, p += u->channels, q++)
                *q = CLAMP(*p, -1.0, 1.0);

            u->descriptor->run(u->handle[c], n);

            q = u->output;
            p = dst + c;
            for (j = 0; j < n; j++, q++, p += u->channels)
                *p = CLAMP(*q, -1.0, 1.0);
        }

        pa_memblock_release(tchunk.memblock);
        pa_memblock_release(u->memchunk.memblock);

        pa_memblock_unref(tchunk.memblock);
    }

    pa_assert(u->memchunk.length > 0);
    pa_assert(u->memchunk.memblock);

    *chunk = u->memchunk;
    pa_memblock_ref(chunk->memblock);

    return 0;
}

/* Called from I/O thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(length > 0);

    if (u->memchunk.memblock) {

        if (length < u->memchunk.length) {
            u->memchunk.index += length;
            u->memchunk.length -= length;
            return;
        }

        pa_memblock_unref(u->memchunk.memblock);
        length -= u->memchunk.length;
        pa_memchunk_reset(&u->memchunk);
    }

    if (length > 0)
        pa_sink_skip(u->sink, length);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_detach_within_thread(u->sink);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_asyncmsgq(u->sink, i->sink->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, i->sink->rtpoll);

    pa_sink_attach_within_thread(u->sink);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_input_unlink(u->sink_input);
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unlink(u->sink);
    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_module_unload_request(u->module);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    char *t;
    pa_sink *master;
    pa_sink_input_new_data data;
    const char *plugin, *label;
    LADSPA_Descriptor_Function descriptor_func;
    const char *e, *cdata;
    const LADSPA_Descriptor *d;
    unsigned long input_port, output_port, p, j, n_control;
    unsigned c;
    pa_bool_t *use_default = NULL;
    char *default_sink_name = NULL;

    pa_assert(m);

    pa_assert(sizeof(LADSPA_Data) == sizeof(float));

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "master", NULL), PA_NAMEREG_SINK, 1))) {
        pa_log("Master sink not found");
        goto fail;
    }

    ss = master->sample_spec;
    ss.format = PA_SAMPLE_FLOAT32;
    map = master->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    if (!(plugin = pa_modargs_get_value(ma, "plugin", NULL))) {
        pa_log("Missing LADSPA plugin name");
        goto fail;
    }

    if (!(label = pa_modargs_get_value(ma, "label", NULL))) {
        pa_log("Missing LADSPA plugin label");
        goto fail;
    }

    cdata = pa_modargs_get_value(ma, "control", NULL);

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->master = master;
    pa_memchunk_reset(&u->memchunk);

    if (!(e = getenv("LADSPA_PATH")))
        e = LADSPA_PATH;

    /* FIXME: This is not exactly thread safe */
    t = pa_xstrdup(lt_dlgetsearchpath());
    lt_dlsetsearchpath(e);
    m->dl = lt_dlopenext(plugin);
    lt_dlsetsearchpath(t);
    pa_xfree(t);

    if (!m->dl) {
        pa_log("Failed to load LADSPA plugin: %s", lt_dlerror());
        goto fail;
    }

    if (!(descriptor_func = (LADSPA_Descriptor_Function) lt_dlsym(m->dl, "ladspa_descriptor"))) {
        pa_log("LADSPA module lacks ladspa_descriptor() symbol.");
        goto fail;
    }

    for (j = 0;; j++) {

        if (!(d = descriptor_func(j))) {
            pa_log("Failed to find plugin label '%s' in plugin '%s'.", plugin, label);
            goto fail;
        }

        if (strcmp(d->Label, label) == 0)
            break;
    }

    u->descriptor = d;

    pa_log_debug("Module: %s", plugin);
    pa_log_debug("Label: %s", d->Label);
    pa_log_debug("Unique ID: %lu", d->UniqueID);
    pa_log_debug("Name: %s", d->Name);
    pa_log_debug("Maker: %s", d->Maker);
    pa_log_debug("Copyright: %s", d->Copyright);

    input_port = output_port = (unsigned long) -1;
    n_control = 0;

    for (p = 0; p < d->PortCount; p++) {

        if (LADSPA_IS_PORT_INPUT(d->PortDescriptors[p]) && LADSPA_IS_PORT_AUDIO(d->PortDescriptors[p])) {

            if (strcmp(d->PortNames[p], "Input") == 0) {
                pa_assert(input_port == (unsigned long) -1);
                input_port = p;
            } else {
                pa_log("Found audio input port on plugin we cannot handle: %s", d->PortNames[p]);
                goto fail;
            }

        } else if (LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p]) && LADSPA_IS_PORT_AUDIO(d->PortDescriptors[p])) {

            if (strcmp(d->PortNames[p], "Output") == 0) {
                pa_assert(output_port == (unsigned long) -1);
                output_port = p;
            } else {
                pa_log("Found audio output port on plugin we cannot handle: %s", d->PortNames[p]);
                goto fail;
            }

        } else if (LADSPA_IS_PORT_INPUT(d->PortDescriptors[p]) && LADSPA_IS_PORT_CONTROL(d->PortDescriptors[p]))
            n_control++;
        else {
            pa_assert(LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p]) && LADSPA_IS_PORT_CONTROL(d->PortDescriptors[p]));
            pa_log_info("Ignored port \"%s\", because we ignore all control out ports.", d->PortNames[p]);
        }
    }

    if ((input_port == (unsigned long) -1) || (output_port == (unsigned long) -1)) {
        pa_log("Failed to identify input and output ports. "
               "Right now this module can only deal with plugins which provide an 'Input' and an 'Output' audio port. "
               "Patches welcome!");
        goto fail;
    }

    u->block_size = pa_frame_align(pa_mempool_block_size_max(m->core->mempool), &ss);

    u->input = (LADSPA_Data*) pa_xnew(uint8_t, u->block_size);
    if (LADSPA_IS_INPLACE_BROKEN(d->Properties))
        u->output = (LADSPA_Data*) pa_xnew(uint8_t, u->block_size);
    else
        u->output = u->input;

    u->channels = ss.channels;

    for (c = 0; c < ss.channels; c++) {
        if (!(u->handle[c] = d->instantiate(d, ss.rate))) {
            pa_log("Failed to instantiate plugin %s with label %s for channel %i", plugin, d->Label, c);
            goto fail;
        }

        d->connect_port(u->handle[c], input_port, u->input);
        d->connect_port(u->handle[c], output_port, u->output);
    }

    if (!cdata && n_control > 0) {
        pa_log("This plugin requires specification of %lu control parameters.", n_control);
        goto fail;
    }

    if (n_control > 0) {
        const char *state = NULL;
        char *k;
        unsigned long h;

        u->control = pa_xnew(LADSPA_Data, n_control);
        use_default = pa_xnew(pa_bool_t, n_control);
        p = 0;

        while ((k = pa_split(cdata, ",", &state))) {
            float f;

            if (*k == 0) {
                use_default[p++] = TRUE;
                pa_xfree(k);
                continue;
            }

            if (pa_atof(k, &f) < 0) {
                pa_log("Failed to parse control value '%s'", k);
                pa_xfree(k);
                goto fail;
            }

            pa_xfree(k);

            if (p >= n_control) {
                pa_log("Too many control values passed, %lu expected.", n_control);
                goto fail;
            }

            use_default[p] = FALSE;
            u->control[p++] = f;
        }

        if (p < n_control) {
            pa_log("Not enough control values passed, %lu expected, %lu passed.", n_control, p);
            goto fail;
        }

        h = 0;
        for (p = 0; p < d->PortCount; p++) {
            LADSPA_PortRangeHintDescriptor hint = d->PortRangeHints[p].HintDescriptor;

            if (!LADSPA_IS_PORT_CONTROL(d->PortDescriptors[p]))
                continue;

            if (LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p])) {
                for (c = 0; c < ss.channels; c++)
                    d->connect_port(u->handle[c], p, &u->control_out);
                continue;
            }

            pa_assert(h < n_control);

            if (use_default[h]) {
                LADSPA_Data lower, upper;

                if (!LADSPA_IS_HINT_HAS_DEFAULT(hint)) {
                    pa_log("Control port value left empty but plugin defines no default.");
                    goto fail;
                }

                lower = d->PortRangeHints[p].LowerBound;
                upper = d->PortRangeHints[p].UpperBound;

                if (LADSPA_IS_HINT_SAMPLE_RATE(hint)) {
                    lower *= ss.rate;
                    upper *= ss.rate;
                }

                switch (hint & LADSPA_HINT_DEFAULT_MASK) {

                    case LADSPA_HINT_DEFAULT_MINIMUM:
                        u->control[h] = lower;
                        break;

                    case LADSPA_HINT_DEFAULT_MAXIMUM:
                        u->control[h] = upper;
                        break;

                    case LADSPA_HINT_DEFAULT_LOW:
                        if (LADSPA_IS_HINT_LOGARITHMIC(hint))
                            u->control[h] = exp(log(lower) * 0.75 + log(upper) * 0.25);
                        else
                            u->control[h] = lower * 0.75 + upper * 0.25;
                        break;

                    case LADSPA_HINT_DEFAULT_MIDDLE:
                        if (LADSPA_IS_HINT_LOGARITHMIC(hint))
                            u->control[h] = exp(log(lower) * 0.5 + log(upper) * 0.5);
                        else
                            u->control[h] = lower * 0.5 + upper * 0.5;
                        break;

                    case LADSPA_HINT_DEFAULT_HIGH:
                        if (LADSPA_IS_HINT_LOGARITHMIC(hint))
                            u->control[h] = exp(log(lower) * 0.25 + log(upper) * 0.75);
                        else
                            u->control[h] = lower * 0.25 + upper * 0.75;
                        break;

                    case LADSPA_HINT_DEFAULT_0:
                        u->control[h] = 0;
                        break;

                    case LADSPA_HINT_DEFAULT_1:
                        u->control[h] = 1;
                        break;

                    case LADSPA_HINT_DEFAULT_100:
                        u->control[h] = 100;
                        break;

                    case LADSPA_HINT_DEFAULT_440:
                        u->control[h] = 440;
                        break;

                    default:
                        pa_assert_not_reached();
                }
            }

            if (LADSPA_IS_HINT_INTEGER(hint))
                u->control[h] = roundf(u->control[h]);

            pa_log_debug("Binding %f to port %s", u->control[h], d->PortNames[p]);

            for (c = 0; c < ss.channels; c++)
                d->connect_port(u->handle[c], p, &u->control[h]);

            h++;
        }

        pa_assert(h == n_control);
    }

    if (d->activate)
        for (c = 0; c < u->channels; c++)
            d->activate(u->handle[c]);

    default_sink_name = pa_sprintf_malloc("%s.ladspa", master->name);

    /* Create sink */
    if (!(u->sink = pa_sink_new(m->core, __FILE__, pa_modargs_get_value(ma, "sink_name", default_sink_name), 0, &ss, &map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state = sink_set_state;
    u->sink->userdata = u;
    u->sink->flags = PA_SINK_LATENCY;

    pa_sink_set_module(u->sink, m);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("LADSPA plugin '%s' on '%s'", label, master->description));
    pa_xfree(t);
    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, master->rtpoll);

    /* Create sink input */
    pa_sink_input_new_data_init(&data);
    data.sink = u->master;
    data.driver = __FILE__;
    data.name = "LADSPA Stream";
    pa_sink_input_new_data_set_sample_spec(&data, &ss);
    pa_sink_input_new_data_set_channel_map(&data, &map);
    data.module = m;

    if (!(u->sink_input = pa_sink_input_new(m->core, &data, PA_SINK_INPUT_DONT_MOVE)))
        goto fail;

    u->sink_input->parent.process_msg = sink_input_process_msg;
    u->sink_input->peek = sink_input_peek_cb;
    u->sink_input->drop = sink_input_drop_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->userdata = u;

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    pa_xfree(use_default);
    pa_xfree(default_sink_name);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(use_default);
    pa_xfree(default_sink_name);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    unsigned c;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }

    if (u->sink) {
        pa_sink_unlink(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    for (c = 0; c < u->channels; c++)
        if (u->handle[c]) {
            if (u->descriptor->deactivate)
                u->descriptor->deactivate(u->handle[c]);
            u->descriptor->cleanup(u->handle[c]);
        }

    if (u->output != u->input)
        pa_xfree(u->output);

    pa_xfree(u->input);

    pa_xfree(u->control);

    pa_xfree(u);
}
