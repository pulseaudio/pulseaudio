/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
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

#include <sndfile.h>

#include <polyp/polyplib.h>
#include <polyp/polyplib-error.h>
#include <polyp/mainloop.h>
#include <polyp/mainloop-signal.h>
#include <polyp/sample.h>

#define BUFSIZE 1024

static struct pa_context *context = NULL;
static struct pa_mainloop_api *mainloop_api = NULL;

static char **process_argv = NULL;

static SNDFILE *sndfile = NULL;
static struct pa_stream *sample_stream = NULL;
static struct pa_sample_spec sample_spec;
static size_t sample_length = 0;

static char *sample_name = NULL;

static enum {
    NONE,
    EXIT,
    STAT,
    UPLOAD_SAMPLE,
    PLAY_SAMPLE,
    REMOVE_SAMPLE
} action = NONE;

static void quit(int ret) {
    assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}


static void context_drain_complete(struct pa_context *c, void *userdata) {
    pa_context_disconnect(c);
}

static void drain(void) {
    struct pa_operation *o;
    if (!(o = pa_context_drain(context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else
        pa_operation_unref(o);
}

static void stat_callback(struct pa_context *c, const struct pa_stat_info *i, void *userdata) {
    if (!i) {
        fprintf(stderr, "Failed to get statistics: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }
    
    fprintf(stderr, "Currently in use: %u blocks containing %u bytes total.\n"
            "Allocated during whole lifetime: %u blocks containing %u bytes total.\n",
            i->memblock_total, i->memblock_total_size, i->memblock_allocated, i->memblock_allocated_size);
    drain();
}

static void play_sample_callback(struct pa_context *c, int success, void *userdata) {
    if (!success) {
        fprintf(stderr, "Failed to play sample: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    drain();
}

static void remove_sample_callback(struct pa_context *c, int success, void *userdata) {
    if (!success) {
        fprintf(stderr, "Failed to remove sample: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    drain();
}

static void stream_state_callback(struct pa_stream *s, void *userdata) {
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

static void stream_write_callback(struct pa_stream *s, size_t length, void *userdata) {
    sf_count_t l;
    float *d;
    assert(s && length && sndfile);

    d = malloc(length);
    assert(d);

    assert(sample_length >= length);
    l = length/pa_frame_size(&sample_spec);

    if ((sf_readf_float(sndfile, d, l)) != l) {
        free(d);
        fprintf(stderr, "Premature end of file\n");
        quit(1);
    }
    
    pa_stream_write(s, d, length, free, 0);

    sample_length -= length;

    if (sample_length  <= 0) {
        pa_stream_set_write_callback(sample_stream, NULL, NULL);
        pa_stream_finish_upload(sample_stream);
    }
}

static void context_state_callback(struct pa_context *c, void *userdata) {
    assert(c);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            if (action == STAT)
                pa_operation_unref(pa_context_stat(c, stat_callback, NULL));
            else if (action == PLAY_SAMPLE)
                pa_operation_unref(pa_context_play_sample(c, process_argv[2], NULL, 0x100, play_sample_callback, NULL));
            else if (action == REMOVE_SAMPLE)
                pa_operation_unref(pa_context_remove_sample(c, process_argv[2], remove_sample_callback, NULL));
            else if (action == UPLOAD_SAMPLE) {

                sample_stream = pa_stream_new(c, sample_name, &sample_spec);
                assert(sample_stream);

                pa_stream_set_state_callback(sample_stream, stream_state_callback, NULL);
                pa_stream_set_write_callback(sample_stream, stream_write_callback, NULL);
                pa_stream_connect_upload(sample_stream, sample_length);
            } else {
                assert(action == EXIT);
                pa_context_exit_daemon(c);
                drain();
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

static void exit_signal_callback(struct pa_mainloop_api *m, struct pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, "Got SIGINT, exiting.\n");
    quit(0);
}

int main(int argc, char *argv[]) {
    struct pa_mainloop* m = NULL;
    char tmp[PATH_MAX];
    
    int ret = 1, r;

    if (argc >= 2) {
        if (!strcmp(argv[1], "stat"))
            action = STAT;
        else if (!strcmp(argv[1], "exit"))
            action = EXIT;
        else if (!strcmp(argv[1], "scache_upload")) {
            struct SF_INFO sfinfo;
            action = UPLOAD_SAMPLE;

            if (argc < 3) {
                fprintf(stderr, "Please specify a sample file to load\n");
                goto quit;
            }

            if (argc >= 4)
                sample_name = argv[3];
            else {
                char *f = strrchr(argv[2], '/');
                size_t n;
                if (f)
                    f++;
                else
                    f = argv[2];

                n = strcspn(f, ".");
                strncpy(sample_name = tmp, f, n);
                tmp[n] = 0; 
            }
            
            memset(&sfinfo, 0, sizeof(sfinfo));
            if (!(sndfile = sf_open(argv[2], SFM_READ, &sfinfo))) {
                fprintf(stderr, "Failed to open sound file.\n");
                goto quit;
            }
            
            sample_spec.format =  PA_SAMPLE_FLOAT32;
            sample_spec.rate = sfinfo.samplerate;
            sample_spec.channels = sfinfo.channels;

            sample_length = sfinfo.frames*pa_frame_size(&sample_spec);
        } else if (!strcmp(argv[1], "scache_play")) {
            action = PLAY_SAMPLE;
            if (argc < 3) {
                fprintf(stderr, "You have to specify a sample name to play\n");
                goto quit;
            }
        } else if (!strcmp(argv[1], "scache_remove")) {
            action = REMOVE_SAMPLE;
            if (argc < 3) {
                fprintf(stderr, "You have to specify a sample name to remove\n");
                goto quit;
            }
        }
    }

    if (action == NONE) {
        fprintf(stderr, "No valid action specified. Use one of: stat, exit, scache_upload, scache_play, scache_remove\n");
        goto quit;
    }

    process_argv = argv;
    
    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(mainloop_api);
    assert(r == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
    signal(SIGPIPE, SIG_IGN);
    
    if (!(context = pa_context_new(mainloop_api, argv[0]))) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);
    pa_context_connect(context, NULL, 1, NULL);

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

    return ret;
}
