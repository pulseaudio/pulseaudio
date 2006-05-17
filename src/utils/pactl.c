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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <getopt.h>

#include <sndfile.h>

#include <polyp/polypaudio.h>

#if PA_API_VERSION != 9
#error Invalid Polypaudio API version
#endif

#define BUFSIZE 1024

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static char *device = NULL, *sample_name = NULL;

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
    LIST
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
        fprintf(stderr, "Failed to get statistics: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    pa_bytes_snprint(s, sizeof(s), i->memblock_total_size);
    printf("Currently in use: %u blocks containing %s bytes total.\n", i->memblock_total, s);

    pa_bytes_snprint(s, sizeof(s), i->memblock_allocated_size);
    printf("Allocated during whole lifetime: %u blocks containing %s bytes total.\n", i->memblock_allocated, s);

    pa_bytes_snprint(s, sizeof(s), i->scache_size);
    printf("Sample cache size: %s\n", s);
    
    complete_action();
}

static void get_server_info_callback(pa_context *c, const pa_server_info *i, void *useerdata) {
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX];
    
    if (!i) {
        fprintf(stderr, "Failed to get server information: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec);

    printf("User name: %s\n"
           "Host Name: %s\n"
           "Server Name: %s\n"
           "Server Version: %s\n"
           "Default Sample Specification: %s\n"
           "Default Sink: %s\n"
           "Default Source: %s\n"
           "Cookie: %08x\n",
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
    
    if (is_last < 0) {
        fprintf(stderr, "Failed to get sink information: %s\n", pa_strerror(pa_context_errno(c)));
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

    printf("*** Sink #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Description: %s\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Owner Module: %u\n"
           "Volume: %s\n"
           "Monitor Source: %u\n"
           "Latency: %0.0f usec\n"
           "Flags: %s%s\n",
           i->index,
           i->name,
           i->driver,
           i->description,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->owner_module,
           i->mute ? "muted" : pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           i->monitor_source,
           (double) i->latency,
           i->flags & PA_SINK_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SINK_LATENCY ? "LATENCY" : "");

}

static void get_source_info_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata) {
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX], t[32], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    if (is_last < 0) {
        fprintf(stderr, "Failed to get source information: %s\n", pa_strerror(pa_context_errno(c)));
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

    snprintf(t, sizeof(t), "%u", i->monitor_of_sink);
    
    printf("*** Source #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Description: %s\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Owner Module: %u\n"
           "Volume: %s\n"
           "Monitor of Sink: %s\n"
           "Latency: %0.0f usec\n"
           "Flags: %s%s\n",
           i->index,
           i->driver,
           i->name,
           i->description,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->owner_module,
           i->mute ? "muted" : pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           i->monitor_of_sink != PA_INVALID_INDEX ? t : "no",
           (double) i->latency,
           i->flags & PA_SOURCE_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SOURCE_LATENCY ? "LATENCY" : "");

}

static void get_module_info_callback(pa_context *c, const pa_module_info *i, int is_last, void *userdata) {
    char t[32];

    if (is_last < 0) {
        fprintf(stderr, "Failed to get module information: %s\n", pa_strerror(pa_context_errno(c)));
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
    
    printf("*** Module #%u ***\n"
           "Name: %s\n"
           "Argument: %s\n"
           "Usage counter: %s\n"
           "Auto unload: %s\n",
           i->index,
           i->name,
           i->argument,
           i->n_used != PA_INVALID_INDEX ? t : "n/a",
           i->auto_unload ? "yes" : "no");
}

static void get_client_info_callback(pa_context *c, const pa_client_info *i, int is_last, void *userdata) {
    char t[32];

    if (is_last < 0) {
        fprintf(stderr, "Failed to get client information: %s\n", pa_strerror(pa_context_errno(c)));
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
    
    printf("*** Client #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Owner Module: %s\n",
           i->index,
           i->name,
           i->driver,
           i->owner_module != PA_INVALID_INDEX ? t : "n/a");
}

static void get_sink_input_info_callback(pa_context *c, const pa_sink_input_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    if (is_last < 0) {
        fprintf(stderr, "Failed to get sink input information: %s\n", pa_strerror(pa_context_errno(c)));
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
    
    printf("*** Sink Input #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Owner Module: %s\n"
           "Client: %s\n"
           "Sink: %u\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Volume: %s\n"
           "Buffer Latency: %0.0f usec\n"
           "Sink Latency: %0.0f usec\n"
           "Resample method: %s\n",
           i->index,
           i->name,
           i->driver,
           i->owner_module != PA_INVALID_INDEX ? t : "n/a",
           i->client != PA_INVALID_INDEX ? k : "n/a",
           i->sink,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           (double) i->buffer_usec,
           (double) i->sink_usec,
           i->resample_method ? i->resample_method : "n/a");
}


static void get_source_output_info_callback(pa_context *c, const pa_source_output_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    if (is_last < 0) {
        fprintf(stderr, "Failed to get source output information: %s\n", pa_strerror(pa_context_errno(c)));
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
    
    printf("*** Source Output #%u ***\n"
           "Name: %s\n"
           "Driver: %s\n"
           "Owner Module: %s\n"
           "Client: %s\n"
           "Source: %u\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Buffer Latency: %0.0f usec\n"
           "Source Latency: %0.0f usec\n"
           "Resample method: %s\n",
           i->index,
           i->name,
           i->driver,
           i->owner_module != PA_INVALID_INDEX ? t : "n/a",
           i->client != PA_INVALID_INDEX ? k : "n/a",
           i->source,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           (double) i->buffer_usec,
           (double) i->source_usec,
           i->resample_method ? i->resample_method : "n/a");
}

static void get_sample_info_callback(pa_context *c, const pa_sample_info *i, int is_last, void *userdata) {
    char t[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    if (is_last < 0) {
        fprintf(stderr, "Failed to get sample information: %s\n", pa_strerror(pa_context_errno(c)));
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
    
    printf("*** Sample #%u ***\n"
           "Name: %s\n"
           "Volume: %s\n"
           "Sample Specification: %s\n"
           "Channel Map: %s\n"
           "Duration: %0.1fs\n"
           "Size: %s\n"
           "Lazy: %s\n"
           "Filename: %s\n",
           i->index,
           i->name,
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           pa_sample_spec_valid(&i->sample_spec) ? pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec) : "n/a",
           pa_sample_spec_valid(&i->sample_spec) ? pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map) : "n/a",
           (double) i->duration/1000000,
           t,
           i->lazy ? "yes" : "no",
           i->filename ? i->filename : "n/a");
}

static void get_autoload_info_callback(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        fprintf(stderr, "Failed to get autoload information: %s\n", pa_strerror(pa_context_errno(c)));
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

    printf("*** Autoload Entry #%u ***\n"
           "Name: %s\n"
           "Type: %s\n"
           "Module: %s\n"
           "Argument: %s\n",
           i->index,
           i->name,
           i->type == PA_AUTOLOAD_SINK ? "sink" : "source",
           i->module,
           i->argument);
}

static void simple_callback(pa_context *c, int success, void *userdata) {
    if (!success) {
        fprintf(stderr, "Failure: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

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
            fprintf(stderr, "Failed to upload sample: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    sf_count_t l;
    float *d;
    assert(s && length && sndfile);

    d = pa_xmalloc(length);

    assert(sample_length >= length);
    l = length/pa_frame_size(&sample_spec);

    if ((sf_readf_float(sndfile, d, l)) != l) {
        pa_xfree(d);
        fprintf(stderr, "Premature end of file\n");
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
                    pa_operation_unref(pa_context_exit_daemon(c, NULL, NULL));
                    drain();

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

                default:
                    assert(0);
            }
            break;

        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Connection failure: %s\n", pa_strerror(pa_context_errno(c)));
            quit(1);
    }
}

static void exit_signal_callback(pa_mainloop_api *m, pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, "Got SIGINT, exiting.\n");
    quit(0);
}

static void help(const char *argv0) {

    printf("%s [options] stat\n"
           "%s [options] list\n"
           "%s [options] exit\n"
           "%s [options] upload-sample FILENAME [NAME]\n"
           "%s [options] play-sample NAME [SINK]\n"
           "%s [options] remove-sample NAME\n\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n\n"
           "  -s, --server=SERVER                   The name of the server to connect to\n"
           "  -n, --client-name=NAME                How to call this client on the server\n",
           argv0, argv0, argv0, argv0, argv0, argv0);
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
                printf("pactl "PACKAGE_VERSION"\nCompiled with libpolyp %s\nLinked with libpolyp %s\n", pa_get_headers_version(), pa_get_library_version());
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
                fprintf(stderr, "Please specify a sample file to load\n");
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
                fprintf(stderr, "Failed to open sound file.\n");
                goto quit;
            }
            
            sample_spec.format =  PA_SAMPLE_FLOAT32;
            sample_spec.rate = sfinfo.samplerate;
            sample_spec.channels = sfinfo.channels;

            sample_length = sfinfo.frames*pa_frame_size(&sample_spec);
        } else if (!strcmp(argv[optind], "play-sample")) {
            action = PLAY_SAMPLE;
            if (optind+1 >= argc) {
                fprintf(stderr, "You have to specify a sample name to play\n");
                goto quit;
            }

            sample_name = pa_xstrdup(argv[optind+1]);

            if (optind+2 < argc)
                device = pa_xstrdup(argv[optind+2]);
            
        } else if (!strcmp(argv[optind], "remove-sample")) {
            action = REMOVE_SAMPLE;
            if (optind+1 >= argc) {
                fprintf(stderr, "You have to specify a sample name to remove\n");
                goto quit;
            }

            sample_name = pa_xstrdup(argv[optind+1]);
        }
    }

    if (action == NONE) {
        fprintf(stderr, "No valid command specified.\n");
        goto quit;
    }

    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
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
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);
    pa_context_connect(context, server, 0, NULL);

    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
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

    return ret;
}
