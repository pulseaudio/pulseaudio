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

static void context_die_callback(struct pa_context *c, void *userdata) {
    assert(c);
    fprintf(stderr, "Connection to server shut down, exiting.\n");
    quit(1);
}

static void context_drain_complete(struct pa_context *c, void *userdata) {
    assert(c);
    fprintf(stderr, "Connection to server shut down, exiting.\n");
    quit(0);
}

static void drain(void) {
    if (pa_context_drain(context, context_drain_complete, NULL) < 0)
        quit(0);
}

static void stat_callback(struct pa_context *c, uint32_t blocks, uint32_t total, void *userdata) {
    if (blocks == (uint32_t) -1) {
        fprintf(stderr, "Failed to get statistics: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }
    
    fprintf(stderr, "Currently in use: %u blocks containing %u bytes total.\n", blocks, total);
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

static void stream_die_callback(struct pa_stream *s, void *userdata) {
    assert(s);
    fprintf(stderr, "Stream deleted, exiting.\n");
    quit(1);
}

static void finish_sample_callback(struct pa_stream *s, int success, void *userdata) {
    assert(s);

    if (!success) {
        fprintf(stderr, "Failed to upload sample: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
        return;
    }

    drain();
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
    
    pa_stream_write(s, d, length);
    free(d);

    sample_length -= length;

    if (sample_length  <= 0) {
        pa_stream_set_write_callback(sample_stream, NULL, NULL);
        pa_stream_finish_sample(sample_stream, finish_sample_callback, NULL);
    }
}

static void upload_callback(struct pa_stream *s, int success, void *userdata) {
    if (!success) {
        fprintf(stderr, "Failed to upload sample: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
    }
}

static void context_complete_callback(struct pa_context *c, int success, void *userdata) {
    assert(c);

    if (!success) {
        fprintf(stderr, "Connection failed: %s\n", pa_strerror(pa_context_errno(c)));
        goto fail;
    }

    fprintf(stderr, "Connection established.\n");

    if (action == STAT)
        pa_context_stat(c, stat_callback, NULL);
    else if (action == PLAY_SAMPLE)
        pa_context_play_sample(c, process_argv[2], NULL, 0x100, play_sample_callback, NULL);
    else if (action == REMOVE_SAMPLE)
        pa_context_remove_sample(c, process_argv[2], remove_sample_callback, NULL);
    else if (action == UPLOAD_SAMPLE) {
        if (!(sample_stream = pa_context_upload_sample(c, sample_name, &sample_spec, sample_length, upload_callback, NULL))) {
            fprintf(stderr, "Failed to upload sample: %s\n", pa_strerror(pa_context_errno(c)));
            goto fail;
        }
        
        pa_stream_set_die_callback(sample_stream, stream_die_callback, NULL);
        pa_stream_set_write_callback(sample_stream, stream_write_callback, NULL);
    } else {
        assert(action == EXIT);
        pa_context_exit(c);
        drain();
    }
    
    return;
    
fail:
    quit(1);
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
                if (f)
                    f++;
                else
                    f = argv[2];

                strncpy(sample_name = tmp, f, strcspn(f, "."));
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

    if (pa_context_connect(context, NULL, context_complete_callback, NULL) < 0) {
        fprintf(stderr, "pa_context_connext() failed.\n");
        goto quit;
    }
        
    pa_context_set_die_callback(context, context_die_callback, NULL);

    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
        goto quit;
    }

quit:
    if (context)
        pa_context_free(context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }
    
    if (sndfile)
        sf_close(sndfile);
    
    return ret;
}
