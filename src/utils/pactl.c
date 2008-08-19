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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <getopt.h>
#include <locale.h>

#include <sndfile.h>

#include <pulse/i18n.h>
#include <pulse/pulseaudio.h>
#include <pulsecore/core-util.h>

#define BUFSIZE 1024

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static char *device = NULL, *sample_name = NULL, *sink_name = NULL, *source_name = NULL, *module_name = NULL, *module_args = NULL;
static uint32_t sink_input_idx = PA_INVALID_INDEX, source_output_idx = PA_INVALID_INDEX;
static uint32_t module_index;
static int suspend;

static SNDFILE *sndfile = NULL;
static pa_stream *sample_stream = NULL;
static pa_sample_spec sample_spec;
static size_t sample_length = 0;

static int actions = 1;

static int nl = 0;

static enum {
    NONE,
    EXIT,
    STAT,
    UPLOAD_SAMPLE,
    PLAY_SAMPLE,
    REMOVE_SAMPLE,
    LIST,
    MOVE_SINK_INPUT,
    MOVE_SOURCE_OUTPUT,
    LOAD_MODULE,
    UNLOAD_MODULE,
    SUSPEND_SINK,
    SUSPEND_SOURCE,
} action = NONE;

static void quit(int ret) {
    assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}


static void context_drain_complete(pa_context *c, void *userdata) {
    pa_context_disconnect(c);
}

static void drain(void) {
    pa_operation *o;
    if (!(o = pa_context_drain(context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else
        pa_operation_unref(o);
}


static void complete_action(void) {
    assert(actions > 0);

    if (!(--actions))
        drain();
}

static void stat_callback(pa_context *c, const pa_stat_info *i, void *userdata) {
    char s[128];
    if (!i) {
        fprintf(stderr, _("Failed to get statistics: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    pa_bytes_snprint(s, sizeof(s), i->memblock_total_size);
    printf(_("Currently in use: %u blocks containing %s bytes total.\n"), i->memblock_total, s);

    pa_bytes_snprint(s, sizeof(s), i->memblock_allocated_size);
    printf(_("Allocated during whole lifetime: %u blocks containing %s bytes total.\n"), i->memblock_allocated, s);

    pa_bytes_snprint(s, sizeof(s), i->scache_size);
    printf(_("Sample cache size: %s\n"), s);

    complete_action();
}

static void get_server_info_callback(pa_context *c, const pa_server_info *i, void *useerdata) {
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX];

    if (!i) {
        fprintf(stderr, _("Failed to get server information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec);

    printf(_("User name: %s\n"
           "Host Name: %s\n"
           "Server Name: %s\n"
           "Server Version: %s\n"
           "Default Sample Specification: %s\n"
           "Default Sink: %s\n"
           "Default Source: %s\n"
           "Cookie: %08x\n"),
           i->user_name,
           i->host_name,
           i->server_name,
           i->server_version,
           s,
           i->default_sink_name,
           i->default_source_name,
           i->cookie);

    complete_action();
}

static void get_sink_info_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata) {
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get sink information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;

    printf(_("*** Sink #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Owner Module: %u\n"
           "Volume: %s\n"
           "Monitor Source: %s\n"
           "Latency: %0.0f usec, configured %0.0f usec\n"
           "Flags: %s%s%s%s%s%s\n"
           "Properties:\n%s"),
           i->index,
           i->name,
           pa_strnull(i->driver),
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->owner_module,
           i->mute ? _("muted") : pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           pa_strnull(i->monitor_source_name),
           (double) i->latency, (double) i->configured_latency,
           i->flags & PA_SINK_HARDWARE ? "HARDWARE " : "",
           i->flags & PA_SINK_NETWORK ? "NETWORK " : "",
           i->flags & PA_SINK_HW_MUTE_CTRL ? "HW_MUTE_CTRL " : "",
           i->flags & PA_SINK_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SINK_DECIBEL_VOLUME ? "DECIBEL_VOLUME " : "",
           i->flags & PA_SINK_LATENCY ? "LATENCY " : "",
           pl = pa_proplist_to_string(i->proplist));

    pa_xfree(pl);
}

static void get_source_info_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata) {
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get source information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;

    printf(_("*** Source #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Owner Module: %u\n"
           "Volume: %s\n"
           "Monitor of Sink: %s\n"
           "Latency: %0.0f usec, configured %0.0f usec\n"
           "Flags: %s%s%s%s%s%s\n"
           "Properties:\n%s"),
           i->index,
           i->name,
           pa_strnull(i->driver),
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->owner_module,
           i->mute ? "muted" : pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           i->monitor_of_sink_name ? i->monitor_of_sink_name : _("n/a"),
           (double) i->latency, (double) i->configured_latency,
           i->flags & PA_SOURCE_HARDWARE ? "HARDWARE " : "",
           i->flags & PA_SOURCE_NETWORK ? "NETWORK " : "",
           i->flags & PA_SOURCE_HW_MUTE_CTRL ? "HW_MUTE_CTRL " : "",
           i->flags & PA_SOURCE_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SOURCE_DECIBEL_VOLUME ? "DECIBEL_VOLUME " : "",
           i->flags & PA_SOURCE_LATENCY ? "LATENCY " : "",
           pl = pa_proplist_to_string(i->proplist));

    pa_xfree(pl);
}

static void get_module_info_callback(pa_context *c, const pa_module_info *i, int is_last, void *userdata) {
    char t[32];

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get module information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;

    snprintf(t, sizeof(t), "%u", i->n_used);

    printf(_("*** Module #%u ***\n"
           "Name: %s\n"
           "Argument: %s\n"
           "Usage counter: %s\n"
           "Auto unload: %s\n"),
           i->index,
           i->name,
           i->argument ? i->argument : "",
           i->n_used != PA_INVALID_INDEX ? t : _("n/a"),
           pa_yes_no(i->auto_unload));
}

static void get_client_info_callback(pa_context *c, const pa_client_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get client information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;

    snprintf(t, sizeof(t), "%u", i->owner_module);

    printf(_("*** Client #%u ***\n"
           "Driver: %s\n"
           "Owner Module: %s\n"
           "Properties:\n%s"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           pl = pa_proplist_to_string(i->proplist));

    pa_xfree(pl);
}

static void get_sink_input_info_callback(pa_context *c, const pa_sink_input_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get sink input information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;

    snprintf(t, sizeof(t), "%u", i->owner_module);
    snprintf(k, sizeof(k), "%u", i->client);

    printf(_("*** Sink Input #%u ***\n"
           "Driver: %s\n"
           "Owner Module: %s\n"
           "Client: %s\n"
           "Sink: %u\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Volume: %s\n"
           "Buffer Latency: %0.0f usec\n"
           "Sink Latency: %0.0f usec\n"
           "Resample method: %s\n"
             "Properties:\n%s"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           i->client != PA_INVALID_INDEX ? k : _("n/a"),
           i->sink,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->mute ? _("muted") : pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           (double) i->buffer_usec,
           (double) i->sink_usec,
           i->resample_method ? i->resample_method : _("n/a"),
           pl = pa_proplist_to_string(i->proplist));

    pa_xfree(pl);
}

static void get_source_output_info_callback(pa_context *c, const pa_source_output_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get source output information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;


    snprintf(t, sizeof(t), "%u", i->owner_module);
    snprintf(k, sizeof(k), "%u", i->client);

    printf(_("*** Source Output #%u ***\n"
           "Driver: %s\n"
           "Owner Module: %s\n"
           "Client: %s\n"
           "Source: %u\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Buffer Latency: %0.0f usec\n"
           "Source Latency: %0.0f usec\n"
           "Resample method: %s\n"
           "Properties:\n%s"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           i->client != PA_INVALID_INDEX ? k : _("n/a"),
           i->source,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           (double) i->buffer_usec,
           (double) i->source_usec,
           i->resample_method ? i->resample_method : _("n/a"),
           pl = pa_proplist_to_string(i->proplist));

    pa_xfree(pl);
}

static void get_sample_info_callback(pa_context *c, const pa_sample_info *i, int is_last, void *userdata) {
    char t[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        fprintf(stderr, _("Failed to get sample information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;


    pa_bytes_snprint(t, sizeof(t), i->bytes);

    printf(_("*** Sample #%u ***\n"
           "Name: %s\n"
           "Volume: %s\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Duration: %0.1fs\n"
           "Size: %s\n"
           "Lazy: %s\n"
           "Filename: %s\n"
           "Properties:\n%s"),
           i->index,
           i->name,
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           pa_sample_spec_valid(&i->sample_spec) ? pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec) : _("n/a"),
           pa_sample_spec_valid(&i->sample_spec) ? pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map) : _("n/a"),
           (double) i->duration/1000000,
           t,
           pa_yes_no(i->lazy),
           i->filename ? i->filename : _("n/a"),
           pl = pa_proplist_to_string(i->proplist));

    pa_xfree(pl);
}

static void get_autoload_info_callback(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        fprintf(stderr, _("Failed to get autoload information: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    assert(i);

    if (nl)
        printf("\n");
    nl = 1;

    printf(_("*** Autoload Entry #%u ***\n"
           "Name: %s\n"
           "Type: %s\n"
           "Module: %s\n"
             "Argument: %s\n"),
           i->index,
           i->name,
           i->type == PA_AUTOLOAD_SINK ? _("sink") : _("source"),
           i->module,
           i->argument ? i->argument : "");
}

static void simple_callback(pa_context *c, int success, void *userdata) {
    if (!success) {
        fprintf(stderr, _("Failure: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    complete_action();
}

static void index_callback(pa_context *c, uint32_t idx, void *userdata) {
    if (idx == PA_INVALID_INDEX) {
        fprintf(stderr, _("Failure: %s\n"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    printf("%u\n", idx);

    complete_action();
}

static void stream_state_callback(pa_stream *s, void *userdata) {
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_READY:
            break;

        case PA_STREAM_TERMINATED:
            drain();
            break;

        case PA_STREAM_FAILED:
        default:
            fprintf(stderr, _("Failed to upload sample: %s\n"), pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    sf_count_t l;
    float *d;
    assert(s && length && sndfile);

    d = pa_xmalloc(length);

    assert(sample_length >= length);
    l = (sf_count_t) (length/pa_frame_size(&sample_spec));

    if ((sf_readf_float(sndfile, d, l)) != l) {
        pa_xfree(d);
        fprintf(stderr, _("Premature end of file\n"));
        quit(1);
    }

    pa_stream_write(s, d, length, pa_xfree, 0, PA_SEEK_RELATIVE);

    sample_length -= length;

    if (sample_length  <= 0) {
        pa_stream_set_write_callback(sample_stream, NULL, NULL);
        pa_stream_finish_upload(sample_stream);
    }
}

static void context_state_callback(pa_context *c, void *userdata) {
    assert(c);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            switch (action) {
                case STAT:
                    actions = 2;
                    pa_operation_unref(pa_context_stat(c, stat_callback, NULL));
                    pa_operation_unref(pa_context_get_server_info(c, get_server_info_callback, NULL));
                    break;

                case PLAY_SAMPLE:
                    pa_operation_unref(pa_context_play_sample(c, sample_name, device, PA_VOLUME_NORM, simple_callback, NULL));
                    break;

                case REMOVE_SAMPLE:
                    pa_operation_unref(pa_context_remove_sample(c, sample_name, simple_callback, NULL));
                    break;

                case UPLOAD_SAMPLE:
                    sample_stream = pa_stream_new(c, sample_name, &sample_spec, NULL);
                    assert(sample_stream);

                    pa_stream_set_state_callback(sample_stream, stream_state_callback, NULL);
                    pa_stream_set_write_callback(sample_stream, stream_write_callback, NULL);
                    pa_stream_connect_upload(sample_stream, sample_length);
                    break;

                case EXIT:
                    pa_operation_unref(pa_context_exit_daemon(c, simple_callback, NULL));
                    break;

                case LIST:
                    actions = 8;
                    pa_operation_unref(pa_context_get_module_info_list(c, get_module_info_callback, NULL));
                    pa_operation_unref(pa_context_get_sink_info_list(c, get_sink_info_callback, NULL));
                    pa_operation_unref(pa_context_get_source_info_list(c, get_source_info_callback, NULL));
                    pa_operation_unref(pa_context_get_sink_input_info_list(c, get_sink_input_info_callback, NULL));
                    pa_operation_unref(pa_context_get_source_output_info_list(c, get_source_output_info_callback, NULL));
                    pa_operation_unref(pa_context_get_client_info_list(c, get_client_info_callback, NULL));
                    pa_operation_unref(pa_context_get_sample_info_list(c, get_sample_info_callback, NULL));
                    pa_operation_unref(pa_context_get_autoload_info_list(c, get_autoload_info_callback, NULL));
                    break;

                case MOVE_SINK_INPUT:
                    pa_operation_unref(pa_context_move_sink_input_by_name(c, sink_input_idx, sink_name, simple_callback, NULL));
                    break;

                case MOVE_SOURCE_OUTPUT:
                    pa_operation_unref(pa_context_move_source_output_by_name(c, source_output_idx, source_name, simple_callback, NULL));
                    break;

                case LOAD_MODULE:
                    pa_operation_unref(pa_context_load_module(c, module_name, module_args, index_callback, NULL));
                    break;

                case UNLOAD_MODULE:
                    pa_operation_unref(pa_context_unload_module(c, module_index, simple_callback, NULL));
                    break;

                case SUSPEND_SINK:
                    if (sink_name)
                        pa_operation_unref(pa_context_suspend_sink_by_name(c, sink_name, suspend, simple_callback, NULL));
                    else
                        pa_operation_unref(pa_context_suspend_sink_by_index(c, PA_INVALID_INDEX, suspend, simple_callback, NULL));
                    break;

                case SUSPEND_SOURCE:
                    if (source_name)
                        pa_operation_unref(pa_context_suspend_source_by_name(c, source_name, suspend, simple_callback, NULL));
                    else
                        pa_operation_unref(pa_context_suspend_source_by_index(c, PA_INVALID_INDEX, suspend, simple_callback, NULL));
                    break;

                default:
                    assert(0);
            }
            break;

        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, _("Connection failure: %s\n"), pa_strerror(pa_context_errno(c)));
            quit(1);
    }
}

static void exit_signal_callback(pa_mainloop_api *m, pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, _("Got SIGINT, exiting.\n"));
    quit(0);
}

static void help(const char *argv0) {

    printf(_("%s [options] stat\n"
           "%s [options] list\n"
           "%s [options] exit\n"
           "%s [options] upload-sample FILENAME [NAME]\n"
           "%s [options] play-sample NAME [SINK]\n"
           "%s [options] remove-sample NAME\n"
           "%s [options] move-sink-input ID SINK\n"
           "%s [options] move-source-output ID SOURCE\n"
           "%s [options] load-module NAME [ARGS ...]\n"
           "%s [options] unload-module ID\n"
           "%s [options] suspend-sink [SINK] 1|0\n"
           "%s [options] suspend-source [SOURCE] 1|0\n\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n\n"
           "  -s, --server=SERVER                   The name of the server to connect to\n"
           "  -n, --client-name=NAME                How to call this client on the server\n"),
           argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

enum { ARG_VERSION = 256 };

int main(int argc, char *argv[]) {
    pa_mainloop* m = NULL;
    char tmp[PATH_MAX];
    int ret = 1, r, c;
    char *server = NULL, *client_name = NULL, *bn;

    static const struct option long_options[] = {
        {"server",      1, NULL, 's'},
        {"client-name", 1, NULL, 'n'},
        {"version",     0, NULL, ARG_VERSION},
        {"help",        0, NULL, 'h'},
        {NULL,          0, NULL, 0}
    };

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, PULSE_LOCALEDIR);

    if (!(bn = strrchr(argv[0], '/')))
        bn = argv[0];
    else
        bn++;

    while ((c = getopt_long(argc, argv, "s:n:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'h' :
                help(bn);
                ret = 0;
                goto quit;

            case ARG_VERSION:
                printf(_("pactl %s\n"
                         "Compiled with libpulse %s\n"
                         "Linked with libpulse %s\n"),
                       PACKAGE_VERSION,
                       pa_get_headers_version(),
                       pa_get_library_version());
                ret = 0;
                goto quit;

            case 's':
                pa_xfree(server);
                server = pa_xstrdup(optarg);
                break;

            case 'n':
                pa_xfree(client_name);
                client_name = pa_xstrdup(optarg);
                break;

            default:
                goto quit;
        }
    }

    if (!client_name)
        client_name = pa_xstrdup(bn);

    if (optind < argc) {
        if (!strcmp(argv[optind], "stat"))
            action = STAT;
        else if (!strcmp(argv[optind], "exit"))
            action = EXIT;
        else if (!strcmp(argv[optind], "list"))
            action = LIST;
        else if (!strcmp(argv[optind], "upload-sample")) {
            struct SF_INFO sfinfo;
            action = UPLOAD_SAMPLE;

            if (optind+1 >= argc) {
                fprintf(stderr, _("Please specify a sample file to load\n"));
                goto quit;
            }

            if (optind+2 < argc)
                sample_name = pa_xstrdup(argv[optind+2]);
            else {
                char *f = strrchr(argv[optind+1], '/');
                size_t n;
                if (f)
                    f++;
                else
                    f = argv[optind];

                n = strcspn(f, ".");
                strncpy(tmp, f, n);
                tmp[n] = 0;
                sample_name = pa_xstrdup(tmp);
            }

            memset(&sfinfo, 0, sizeof(sfinfo));
            if (!(sndfile = sf_open(argv[optind+1], SFM_READ, &sfinfo))) {
                fprintf(stderr, _("Failed to open sound file.\n"));
                goto quit;
            }

            sample_spec.format = PA_SAMPLE_FLOAT32;
            sample_spec.rate = (uint32_t) sfinfo.samplerate;
            sample_spec.channels = (uint8_t) sfinfo.channels;

            sample_length = (size_t)sfinfo.frames*pa_frame_size(&sample_spec);
        } else if (!strcmp(argv[optind], "play-sample")) {
            action = PLAY_SAMPLE;
            if (argc != optind+2 && argc != optind+3) {
                fprintf(stderr, _("You have to specify a sample name to play\n"));
                goto quit;
            }

            sample_name = pa_xstrdup(argv[optind+1]);

            if (optind+2 < argc)
                device = pa_xstrdup(argv[optind+2]);

        } else if (!strcmp(argv[optind], "remove-sample")) {
            action = REMOVE_SAMPLE;
            if (argc != optind+2) {
                fprintf(stderr, _("You have to specify a sample name to remove\n"));
                goto quit;
            }

            sample_name = pa_xstrdup(argv[optind+1]);
        } else if (!strcmp(argv[optind], "move-sink-input")) {
            action = MOVE_SINK_INPUT;
            if (argc != optind+3) {
                fprintf(stderr, _("You have to specify a sink input index and a sink\n"));
                goto quit;
            }

            sink_input_idx = (uint32_t) atoi(argv[optind+1]);
            sink_name = pa_xstrdup(argv[optind+2]);
        } else if (!strcmp(argv[optind], "move-source-output")) {
            action = MOVE_SOURCE_OUTPUT;
            if (argc != optind+3) {
                fprintf(stderr, _("You have to specify a source output index and a source\n"));
                goto quit;
            }

            source_output_idx = (uint32_t) atoi(argv[optind+1]);
            source_name = pa_xstrdup(argv[optind+2]);
        } else if (!strcmp(argv[optind], "load-module")) {
            int i;
            size_t n = 0;
            char *p;

            action = LOAD_MODULE;

            if (argc <= optind+1) {
                fprintf(stderr, _("You have to specify a module name and arguments.\n"));
                goto quit;
            }

            module_name = argv[optind+1];

            for (i = optind+2; i < argc; i++)
                n += strlen(argv[i])+1;

            if (n > 0) {
                p = module_args = pa_xmalloc(n);

                for (i = optind+2; i < argc; i++)
                    p += sprintf(p, "%s%s", p == module_args ? "" : " ", argv[i]);
            }

        } else if (!strcmp(argv[optind], "unload-module")) {
            action = UNLOAD_MODULE;

            if (argc != optind+2) {
                fprintf(stderr, _("You have to specify a module index\n"));
                goto quit;
            }

            module_index = (uint32_t) atoi(argv[optind+1]);

        } else if (!strcmp(argv[optind], "suspend-sink")) {
            action = SUSPEND_SINK;

            if (argc > optind+3 || optind+1 >= argc) {
                fprintf(stderr, _("You may not specify more than one sink. You have to specify at least one boolean value.\n"));
                goto quit;
            }

            suspend = pa_parse_boolean(argv[argc-1]);

            if (argc > optind+2)
                sink_name = pa_xstrdup(argv[optind+1]);

        } else if (!strcmp(argv[optind], "suspend-source")) {
            action = SUSPEND_SOURCE;

            if (argc > optind+3 || optind+1 >= argc) {
                fprintf(stderr, _("You may not specify more than one source. You have to specify at least one boolean value.\n"));
                goto quit;
            }

            suspend = pa_parse_boolean(argv[argc-1]);

            if (argc > optind+2)
                source_name = pa_xstrdup(argv[optind+1]);
        } else if (!strcmp(argv[optind], "help")) {
            help(bn);
            ret = 0;
            goto quit;
        }
    }

    if (action == NONE) {
        fprintf(stderr, _("No valid command specified.\n"));
        goto quit;
    }

    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, _("pa_mainloop_new() failed.\n"));
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(mainloop_api);
    assert(r == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    if (!(context = pa_context_new(mainloop_api, client_name))) {
        fprintf(stderr, _("pa_context_new() failed.\n"));
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);
    pa_context_connect(context, server, 0, NULL);

    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, _("pa_mainloop_run() failed.\n"));
        goto quit;
    }

quit:
    if (sample_stream)
        pa_stream_unref(sample_stream);

    if (context)
        pa_context_unref(context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }

    if (sndfile)
        sf_close(sndfile);

    pa_xfree(server);
    pa_xfree(device);
    pa_xfree(sample_name);
    pa_xfree(sink_name);
    pa_xfree(source_name);
    pa_xfree(module_args);
    pa_xfree(client_name);

    return ret;
}
