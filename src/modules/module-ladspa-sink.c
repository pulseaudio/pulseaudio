/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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
#include <pulse/i18n.h>

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
#include <pulsecore/ltdl-helper.h>

#include "module-ladspa-sink-symdef.h"
#include "ladspa.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION(_("Virtual LADSPA sink"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        _("sink_name=<name for the sink> "
          "sink_properties=<properties for the sink> "
          "master=<name of sink to filter> "
          "format=<sample format> "
          "rate=<sample rate> "
          "channels=<number of channels> "
          "channel_map=<channel map> "
          "plugin=<ladspa plugin name> "
          "label=<ladspa plugin label> "
          "control=<comma seperated list of input control values>"));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)

struct userdata {
    pa_module *module;

    pa_sink *sink;
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

    pa_memblockq *memblockq;

    pa_bool_t auto_desc;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "master",
    "format",
    "rate",
    "channels",
    "channel_map",
    "plugin",
    "label",
    "control",
    NULL
};

/* Called from I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY:

            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it in that time. Also, the
             * sink input is first shut down, the sink second. */
            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
                !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =

                /* Get the latency of the master sink */
                pa_sink_get_latency_within_thread(u->sink_input->sink) +

                /* Add the latency internal to our sink input on top */
                pa_bytes_to_usec(pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq), &u->sink_input->sink->sample_spec);

            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_cb(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(state) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return 0;

    pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);
    return 0;
}

/* Called from I/O thread context */
static void sink_request_rewind_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input, s->thread_info.rewind_nbytes + pa_memblockq_get_length(u->memblockq), TRUE, FALSE, FALSE);
}

/* Called from I/O thread context */
static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
            u->sink_input,
            pa_sink_get_requested_latency_within_thread(s));
}

/* Called from main context */
static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return;

    pa_sink_input_set_volume(u->sink_input, &s->real_volume, s->save_volume, TRUE);
}

/* Called from main context */
static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return;

    pa_sink_input_set_mute(u->sink_input, s->muted, s->save_muted);
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;
    float *src, *dst;
    size_t fs;
    unsigned n, c;
    pa_memchunk tchunk;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = i->userdata);

    /* Hmm, process any rewind request that might be queued up */
    pa_sink_process_rewind(u->sink, 0);

    while (pa_memblockq_peek(u->memblockq, &tchunk) < 0) {
        pa_memchunk nchunk;

        pa_sink_render(u->sink, nbytes, &nchunk);
        pa_memblockq_push(u->memblockq, &nchunk);
        pa_memblock_unref(nchunk.memblock);
    }

    tchunk.length = PA_MIN(nbytes, tchunk.length);
    pa_assert(tchunk.length > 0);

    fs = pa_frame_size(&i->sample_spec);
    n = (unsigned) (PA_MIN(tchunk.length, u->block_size) / fs);

    pa_assert(n > 0);

    chunk->index = 0;
    chunk->length = n*fs;
    chunk->memblock = pa_memblock_new(i->sink->core->mempool, chunk->length);

    pa_memblockq_drop(u->memblockq, chunk->length);

    src = (float*) ((uint8_t*) pa_memblock_acquire(tchunk.memblock) + tchunk.index);
    dst = (float*) pa_memblock_acquire(chunk->memblock);

    for (c = 0; c < u->channels; c++) {
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE, u->input, sizeof(float), src+c, u->channels*sizeof(float), n);
        u->descriptor->run(u->handle[c], n);
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE, dst+c, u->channels*sizeof(float), u->output, sizeof(float), n);
    }

    pa_memblock_release(tchunk.memblock);
    pa_memblock_release(chunk->memblock);

    pa_memblock_unref(tchunk.memblock);

    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t amount = 0;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->sink->thread_info.rewind_nbytes > 0) {
        size_t max_rewrite;

        max_rewrite = nbytes + pa_memblockq_get_length(u->memblockq);
        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, max_rewrite);
        u->sink->thread_info.rewind_nbytes = 0;

        if (amount > 0) {
            unsigned c;

            pa_memblockq_seek(u->memblockq, - (int64_t) amount, PA_SEEK_RELATIVE, TRUE);

            pa_log_debug("Resetting plugin");

            /* Reset the plugin */
            if (u->descriptor->deactivate)
                for (c = 0; c < u->channels; c++)
                    u->descriptor->deactivate(u->handle[c]);
            if (u->descriptor->activate)
                for (c = 0; c < u->channels; c++)
                    u->descriptor->activate(u->handle[c]);
        }
    }

    pa_sink_process_rewind(u->sink, amount);
    pa_memblockq_rewind(u->memblockq, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_set_maxrewind(u->memblockq, nbytes);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_max_request_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_detach_within_thread(u->sink);

    pa_sink_set_rtpoll(u->sink, NULL);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
    pa_sink_set_max_request_within_thread(u->sink, pa_sink_input_get_max_request(i));
    pa_sink_set_max_rewind_within_thread(u->sink, pa_sink_input_get_max_rewind(i));

    pa_sink_attach_within_thread(u->sink);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* The order here matters! We first kill the sink input, followed
     * by the sink. That means the sink callbacks must be protected
     * against an unconnected sink input! */
    pa_sink_input_unlink(u->sink_input);
    pa_sink_unlink(u->sink);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_module_unload_request(u->module, TRUE);
}

/* Called from IO thread context */
static void sink_input_state_change_cb(pa_sink_input *i, pa_sink_input_state_t state) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* If we are added for the first time, ask for a rewinding so that
     * we are heard right-away. */
    if (PA_SINK_INPUT_IS_LINKED(state) &&
        i->thread_info.state == PA_SINK_INPUT_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(i, 0, FALSE, TRUE, TRUE);
    }
}

/* Called from main context */
static pa_bool_t sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    return u->sink != dest;
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (dest) {
        pa_sink_set_asyncmsgq(u->sink, dest->asyncmsgq);
        pa_sink_update_flags(u->sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, dest->flags);
    } else
        pa_sink_set_asyncmsgq(u->sink, NULL);

    if (u->auto_desc && dest) {
        const char *z;
        pa_proplist *pl;

        pl = pa_proplist_new();
        z = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(pl, PA_PROP_DEVICE_DESCRIPTION, "LADSPA Plugin %s on %s",
                         pa_proplist_gets(u->sink->proplist, "device.ladspa.name"), z ? z : dest->name);

        pa_sink_update_proplist(u->sink, PA_UPDATE_REPLACE, pl);
        pa_proplist_free(pl);
    }
}

/* Called from main context */
static void sink_input_volume_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_volume_changed(u->sink, &i->volume);
}

/* Called from main context */
static void sink_input_mute_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_mute_changed(u->sink, i->muted);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    char *t;
    pa_sink *master;
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;
    const char *plugin, *label;
    LADSPA_Descriptor_Function descriptor_func;
    const char *e, *cdata;
    const LADSPA_Descriptor *d;
    unsigned long input_port, output_port, p, j, n_control;
    unsigned c;
    pa_bool_t *use_default = NULL;

    pa_assert(m);

    pa_assert_cc(sizeof(LADSPA_Data) == sizeof(float));

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "master", NULL), PA_NAMEREG_SINK))) {
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
    u->module = m;
    m->userdata = u;
    u->memblockq = pa_memblockq_new(0, MEMBLOCKQ_MAXLENGTH, 0, pa_frame_size(&ss), 1, 1, 0, NULL);

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

    if (!(descriptor_func = (LADSPA_Descriptor_Function) pa_load_sym(m->dl, NULL, "ladspa_descriptor"))) {
        pa_log("LADSPA module lacks ladspa_descriptor() symbol.");
        goto fail;
    }

    for (j = 0;; j++) {

        if (!(d = descriptor_func(j))) {
            pa_log("Failed to find plugin label '%s' in plugin '%s'.", label, plugin);
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
            pa_log_debug("Ignored control output port \"%s\".", d->PortNames[p]);
        }
    }

    if ((input_port == (unsigned long) -1) || (output_port == (unsigned long) -1)) {
        pa_log("Failed to identify input and output ports. "
               "Right now this module can only deal with plugins which provide an 'Input' and an 'Output' audio port. "
               "Patches welcome!");
        goto fail;
    }

    u->block_size = pa_frame_align(pa_mempool_block_size_max(m->core->mempool), &ss);

    u->input = (LADSPA_Data*) pa_xnew(uint8_t, (unsigned) u->block_size);
    if (LADSPA_IS_INPLACE_BROKEN(d->Properties))
        u->output = (LADSPA_Data*) pa_xnew(uint8_t, (unsigned) u->block_size);
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

        u->control = pa_xnew(LADSPA_Data, (unsigned) n_control);
        use_default = pa_xnew(pa_bool_t, (unsigned) n_control);
        p = 0;

        while ((k = pa_split(cdata, ",", &state)) && p < n_control) {
            double f;

            if (*k == 0) {
                use_default[p++] = TRUE;
                pa_xfree(k);
                continue;
            }

            if (pa_atod(k, &f) < 0) {
                pa_log("Failed to parse control value '%s'", k);
                pa_xfree(k);
                goto fail;
            }

            pa_xfree(k);

            use_default[p] = FALSE;
            u->control[p++] = (LADSPA_Data) f;
        }

        /* The previous loop doesn't take the last control value into account
           if it is left empty, so we do it here. */
        if (*cdata == 0 || cdata[strlen(cdata) - 1] == ',') {
            if (p < n_control)
                use_default[p] = TRUE;
            p++;
        }

        if (p > n_control || k) {
            pa_log("Too many control values passed, %lu expected.", n_control);
            pa_xfree(k);
            goto fail;
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
                    lower *= (LADSPA_Data) ss.rate;
                    upper *= (LADSPA_Data) ss.rate;
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
                            u->control[h] = (LADSPA_Data) exp(log(lower) * 0.75 + log(upper) * 0.25);
                        else
                            u->control[h] = (LADSPA_Data) (lower * 0.75 + upper * 0.25);
                        break;

                    case LADSPA_HINT_DEFAULT_MIDDLE:
                        if (LADSPA_IS_HINT_LOGARITHMIC(hint))
                            u->control[h] = (LADSPA_Data) exp(log(lower) * 0.5 + log(upper) * 0.5);
                        else
                            u->control[h] = (LADSPA_Data) (lower * 0.5 + upper * 0.5);
                        break;

                    case LADSPA_HINT_DEFAULT_HIGH:
                        if (LADSPA_IS_HINT_LOGARITHMIC(hint))
                            u->control[h] = (LADSPA_Data) exp(log(lower) * 0.25 + log(upper) * 0.75);
                        else
                            u->control[h] = (LADSPA_Data) (lower * 0.25 + upper * 0.75);
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

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    if (!(sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        sink_data.name = pa_sprintf_malloc("%s.ladspa", master->name);
    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");
    pa_proplist_sets(sink_data.proplist, "device.ladspa.module", plugin);
    pa_proplist_sets(sink_data.proplist, "device.ladspa.label", d->Label);
    pa_proplist_sets(sink_data.proplist, "device.ladspa.name", d->Name);
    pa_proplist_sets(sink_data.proplist, "device.ladspa.maker", d->Maker);
    pa_proplist_sets(sink_data.proplist, "device.ladspa.copyright", d->Copyright);
    pa_proplist_setf(sink_data.proplist, "device.ladspa.unique_id", "%lu", (unsigned long) d->UniqueID);

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    if ((u->auto_desc = !pa_proplist_contains(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION))) {
        const char *z;

        z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "LADSPA Plugin %s on %s", d->Name, z ? z : master->name);
    }

    u->sink = pa_sink_new(m->core, &sink_data,
                          PA_SINK_HW_MUTE_CTRL|PA_SINK_HW_VOLUME_CTRL|PA_SINK_DECIBEL_VOLUME|
                          (master->flags & (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY)));
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg_cb;
    u->sink->set_state = sink_set_state_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->request_rewind = sink_request_rewind_cb;
    u->sink->set_volume = sink_set_volume_cb;
    u->sink->set_mute = sink_set_mute_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    sink_input_data.sink = master;
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "LADSPA Stream");
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    u->sink_input->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->state_change = sink_input_state_change_cb;
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->volume_changed = sink_input_volume_changed_cb;
    u->sink_input->mute_changed = sink_input_mute_changed_cb;
    u->sink_input->userdata = u;

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    pa_xfree(use_default);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(use_default);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;
    unsigned c;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    /* See comments in sink_input_kill_cb() above regarding
     * destruction order! */

    if (u->sink_input)
        pa_sink_input_unlink(u->sink_input);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->sink_input)
        pa_sink_input_unref(u->sink_input);

    if (u->sink)
        pa_sink_unref(u->sink);

    for (c = 0; c < u->channels; c++)
        if (u->handle[c]) {
            if (u->descriptor->deactivate)
                u->descriptor->deactivate(u->handle[c]);
            u->descriptor->cleanup(u->handle[c]);
        }

    if (u->output != u->input)
        pa_xfree(u->output);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    pa_xfree(u->input);

    pa_xfree(u->control);

    pa_xfree(u);
}
