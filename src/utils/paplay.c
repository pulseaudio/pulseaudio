/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <getopt.h>
#include <locale.h>

#include <sndfile.h>

#include <pulse/pulseaudio.h>
#include <pulse/i18n.h>

static pa_context *context = NULL;
static pa_stream *stream = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static char *stream_name = NULL, *client_name = NULL, *device = NULL;

static int verbose = 0;
static pa_volume_t volume = PA_VOLUME_NORM;

static SNDFILE* sndfile = NULL;

static pa_sample_spec sample_spec = { 0, 0, 0 };
static pa_channel_map channel_map;
static int channel_map_set = 0;

static sf_count_t (*readf_function)(SNDFILE *_sndfile, void *ptr, sf_count_t frames) = NULL;

/* A shortcut for terminating the application */
static void quit(int ret) {
    assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}

/* Connection draining complete */
static void context_drain_complete(pa_context*c, void *userdata) {
    pa_context_disconnect(c);
}

/* Stream draining complete */
static void stream_drain_complete(pa_stream*s, int success, void *userdata) {
    pa_operation *o;

    if (!success) {
        fprintf(stderr, _("Failed to drain stream: %s\n"), pa_strerror(pa_context_errno(context)));
        quit(1);
    }

    if (verbose)
        fprintf(stderr, _("Playback stream drained.\n"));

    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    stream = NULL;

    if (!(o = pa_context_drain(context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else {
        pa_operation_unref(o);

        if (verbose)
            fprintf(stderr, _("Draining connection to server.\n"));
    }
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    sf_count_t bytes;
    void *data;
    assert(s && length);

    if (!sndfile)
        return;

    data = pa_xmalloc(length);

    if (readf_function) {
        size_t k = pa_frame_size(&sample_spec);

        if ((bytes = readf_function(sndfile, data, (sf_count_t) (length/k))) > 0)
            bytes *= (sf_count_t) k;

    } else
        bytes = sf_read_raw(sndfile, data, (sf_count_t) length);

    if (bytes > 0)
        pa_stream_write(s, data, (size_t) bytes, pa_xfree, 0, PA_SEEK_RELATIVE);
    else
        pa_xfree(data);

    if (bytes < (sf_count_t) length) {
        sf_close(sndfile);
        sndfile = NULL;
        pa_operation_unref(pa_stream_drain(s, stream_drain_complete, NULL));
    }
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_TERMINATED:
            break;

        case PA_STREAM_READY:
            if (verbose)
                fprintf(stderr, _("Stream successfully created\n"));
            break;

        case PA_STREAM_FAILED:
        default:
            fprintf(stderr, _("Stream errror: %s\n"), pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {
            pa_cvolume cv;

            assert(c && !stream);

            if (verbose)
                fprintf(stderr, _("Connection established.\n"));

            stream = pa_stream_new(c, stream_name, &sample_spec, channel_map_set ? &channel_map : NULL);
            assert(stream);

            pa_stream_set_state_callback(stream, stream_state_callback, NULL);
            pa_stream_set_write_callback(stream, stream_write_callback, NULL);
            pa_stream_connect_playback(stream, device, NULL, 0, pa_cvolume_set(&cv, sample_spec.channels, volume), NULL);

            break;
        }

        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, _("Connection failure: %s\n"), pa_strerror(pa_context_errno(c)));
            quit(1);
    }
}

/* UNIX signal to quit recieved */
static void exit_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata) {
    if (verbose)
        fprintf(stderr, _("Got SIGINT, exiting.\n"));
    quit(0);

}

static void help(const char *argv0) {

    printf(_("%s [options] [FILE]\n\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n\n"
           "  -v, --verbose                         Enable verbose operation\n\n"
           "  -s, --server=SERVER                   The name of the server to connect to\n"
           "  -d, --device=DEVICE                   The name of the sink to connect to\n"
           "  -n, --client-name=NAME                How to call this client on the server\n"
           "      --stream-name=NAME                How to call this stream on the server\n"
           "      --volume=VOLUME                   Specify the initial (linear) volume in range 0...65536\n"
             "      --channel-map=CHANNELMAP          Set the channel map to the use\n"),
           argv0);
}

enum {
    ARG_VERSION = 256,
    ARG_STREAM_NAME,
    ARG_VOLUME,
    ARG_CHANNELMAP
};

int main(int argc, char *argv[]) {
    pa_mainloop* m = NULL;
    int ret = 1, r, c;
    char *bn, *server = NULL;
    const char *filename;
    SF_INFO sfinfo;

    static const struct option long_options[] = {
        {"device",      1, NULL, 'd'},
        {"server",      1, NULL, 's'},
        {"client-name", 1, NULL, 'n'},
        {"stream-name", 1, NULL, ARG_STREAM_NAME},
        {"version",     0, NULL, ARG_VERSION},
        {"help",        0, NULL, 'h'},
        {"verbose",     0, NULL, 'v'},
        {"volume",      1, NULL, ARG_VOLUME},
        {"channel-map", 1, NULL, ARG_CHANNELMAP},
        {NULL,          0, NULL, 0}
    };

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, PULSE_LOCALEDIR);

    if (!(bn = strrchr(argv[0], '/')))
        bn = argv[0];
    else
        bn++;

    while ((c = getopt_long(argc, argv, "d:s:n:hv", long_options, NULL)) != -1) {

        switch (c) {
            case 'h' :
                help(bn);
                ret = 0;
                goto quit;

            case ARG_VERSION:
                printf(_("paplay %s\nCompiled with libpulse %s\n"
                        "Linked with libpulse %s\n"), PACKAGE_VERSION, pa_get_headers_version(), pa_get_library_version());
                ret = 0;
                goto quit;

            case 'd':
                pa_xfree(device);
                device = pa_xstrdup(optarg);
                break;

            case 's':
                pa_xfree(server);
                server = pa_xstrdup(optarg);
                break;

            case 'n':
                pa_xfree(client_name);
                client_name = pa_xstrdup(optarg);
                break;

            case ARG_STREAM_NAME:
                pa_xfree(stream_name);
                stream_name = pa_xstrdup(optarg);
                break;

            case 'v':
                verbose = 1;
                break;

            case ARG_VOLUME: {
                int v = atoi(optarg);
                volume = v < 0 ? 0U : (pa_volume_t) v;
                break;
            }

            case ARG_CHANNELMAP:
                if (!pa_channel_map_parse(&channel_map, optarg)) {
                    fprintf(stderr, _("Invalid channel map\n"));
                    goto quit;
                }

                channel_map_set = 1;
                break;

            default:
                goto quit;
        }
    }

    filename = optind < argc ? argv[optind] : "STDIN";

    memset(&sfinfo, 0, sizeof(sfinfo));

    if (optind < argc)
        sndfile = sf_open(filename, SFM_READ, &sfinfo);
    else
        sndfile = sf_open_fd(STDIN_FILENO, SFM_READ, &sfinfo, 0);

    if (!sndfile) {
        fprintf(stderr, _("Failed to open file '%s'\n"), filename);
        goto quit;
    }

    sample_spec.rate = (uint32_t) sfinfo.samplerate;
    sample_spec.channels = (uint8_t) sfinfo.channels;

    readf_function = NULL;

    switch (sfinfo.format & 0xFF) {
        case SF_FORMAT_PCM_16:
        case SF_FORMAT_PCM_U8:
        case SF_FORMAT_PCM_S8:
            sample_spec.format = PA_SAMPLE_S16NE;
            readf_function = (sf_count_t (*)(SNDFILE *_sndfile, void *ptr, sf_count_t frames)) sf_readf_short;
            break;

        case SF_FORMAT_ULAW:
            sample_spec.format = PA_SAMPLE_ULAW;
            break;

        case SF_FORMAT_ALAW:
            sample_spec.format = PA_SAMPLE_ALAW;
            break;

        case SF_FORMAT_FLOAT:
        case SF_FORMAT_DOUBLE:
        default:
            sample_spec.format = PA_SAMPLE_FLOAT32NE;
            readf_function = (sf_count_t (*)(SNDFILE *_sndfile, void *ptr, sf_count_t frames)) sf_readf_float;
            break;
    }

    assert(pa_sample_spec_valid(&sample_spec));

    if (channel_map_set && channel_map.channels != sample_spec.channels) {
        fprintf(stderr, _("Channel map doesn't match file.\n"));
        goto quit;
    }

    if (!client_name) {
        client_name = pa_locale_to_utf8(bn);
        if (!client_name)
            client_name = pa_utf8_filter(bn);
    }

    if (!stream_name) {
        const char *n;

        n = sf_get_string(sndfile, SF_STR_TITLE);

        if (!n)
            n = filename;

        stream_name = pa_locale_to_utf8(n);
        if (!stream_name)
            stream_name = pa_utf8_filter(n);
    }

    if (verbose) {
        char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(t, sizeof(t), &sample_spec);
        fprintf(stderr, _("Using sample spec '%s'\n"), t);
    }

    /* Set up a new main loop */
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

    /* Create a new connection context */
    if (!(context = pa_context_new(mainloop_api, client_name))) {
        fprintf(stderr, _("pa_context_new() failed.\n"));
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);

    /* Connect the context */
    if (pa_context_connect(context, server, 0, NULL) < 0) {
        fprintf(stderr, _("pa_context_connect() failed: %s"), pa_strerror(pa_context_errno(context)));
        goto quit;
    }

    /* Run the main loop */
    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, _("pa_mainloop_run() failed.\n"));
        goto quit;
    }

quit:
    if (stream)
        pa_stream_unref(stream);

    if (context)
        pa_context_unref(context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }

    pa_xfree(server);
    pa_xfree(device);
    pa_xfree(client_name);
    pa_xfree(stream_name);

    if (sndfile)
        sf_close(sndfile);

    return ret;
}
