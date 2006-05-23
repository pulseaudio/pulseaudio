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

#ifdef _FILE_OFFSET_BITS
#undef _FILE_OFFSET_BITS
#endif

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif

#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>

#include <linux/sockios.h>

#include <polyp/polypaudio.h>
#include <polypcore/llist.h>
#include <polypcore/gccmacro.h>

typedef enum {
    FD_INFO_MIXER,
    FD_INFO_PLAYBACK
} fd_info_type_t;

typedef struct fd_info fd_info;

struct fd_info {
    pthread_mutex_t mutex;
    int ref;
    
    fd_info_type_t type;
    int app_fd, thread_fd;

    pa_sample_spec sample_spec;
    size_t fragment_size;
    unsigned n_fragments;

    pa_threaded_mainloop *mainloop;
    pa_context *context;
    pa_stream *stream;

    pa_io_event *io_event;

    void *buf;

    int operation_success;

    PA_LLIST_FIELDS(fd_info);
};

static int dsp_drain(fd_info *i);

static pthread_mutex_t fd_infos_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t func_mutex = PTHREAD_MUTEX_INITIALIZER;

static PA_LLIST_HEAD(fd_info, fd_infos) = NULL;

static int (*_ioctl)(int, int, void*) = NULL;
static int (*_close)(int) = NULL;
static int (*_open)(const char *, int, mode_t) = NULL;
static FILE* (*_fopen)(const char *path, const char *mode) = NULL;
static int (*_open64)(const char *, int, mode_t) = NULL;
static FILE* (*_fopen64)(const char *path, const char *mode) = NULL;
static int (*_fclose)(FILE *f) = NULL;

#define LOAD_IOCTL_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_ioctl)  \
        _ioctl = (int (*)(int, int, void*)) dlsym(RTLD_NEXT, "ioctl"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

#define LOAD_OPEN_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_open) \
        _open = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "open"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

#define LOAD_OPEN64_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_open64) \
        _open64 = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "open64"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

#define LOAD_CLOSE_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_close) \
        _close = (int (*)(int)) dlsym(RTLD_NEXT, "close"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

#define LOAD_FOPEN_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_fopen) \
        _fopen = (FILE* (*)(const char *, const char*)) dlsym(RTLD_NEXT, "fopen"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

#define LOAD_FOPEN64_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_fopen64) \
        _fopen64 = (FILE* (*)(const char *, const char*)) dlsym(RTLD_NEXT, "fopen64"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

#define LOAD_FCLOSE_FUNC() \
do { \
    pthread_mutex_lock(&func_mutex); \
    if (!_fclose) \
        _fclose = (int (*)(FILE *)) dlsym(RTLD_NEXT, "fclose"); \
    pthread_mutex_unlock(&func_mutex); \
} while(0)

static void debug(const char *format, ...) PA_GCC_PRINTF_ATTR(1,2);

static void debug(const char *format, ...) {
    va_list ap;
    if (getenv("PADSP_DEBUG")) {
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

static pthread_key_t recursion_key;

static void recursion_key_alloc(void) {
    pthread_key_create(&recursion_key, NULL);
}

static int function_enter(void) {
    /* Avoid recursive calls */
    static pthread_once_t recursion_key_once = PTHREAD_ONCE_INIT;
    pthread_once(&recursion_key_once, recursion_key_alloc);
    
    if (pthread_getspecific(recursion_key))
        return 0;

    pthread_setspecific(recursion_key, (void*) 1);
    return 1;
}

static void function_exit(void) {
    pthread_setspecific(recursion_key, NULL);
}

static void fd_info_free(fd_info *i) {
    assert(i);

    debug(__FILE__": freeing fd info (fd=%i)\n", i->app_fd);

    dsp_drain(i);
    
    if (i->mainloop)
        pa_threaded_mainloop_stop(i->mainloop);
    
    if (i->stream) {
        pa_stream_disconnect(i->stream);
        pa_stream_unref(i->stream);
    }

    if (i->context) {
        pa_context_disconnect(i->context);
        pa_context_unref(i->context);
    }
    
    if (i->mainloop)
        pa_threaded_mainloop_free(i->mainloop);

    if (i->app_fd >= 0) {
        LOAD_CLOSE_FUNC();
        _close(i->app_fd);
    }

    if (i->thread_fd >= 0) {
        LOAD_CLOSE_FUNC();
        _close(i->thread_fd);
    }

    free(i->buf);

    pthread_mutex_destroy(&i->mutex);
    free(i);
}

static fd_info *fd_info_ref(fd_info *i) {
    assert(i);
    
    pthread_mutex_lock(&i->mutex);
    assert(i->ref >= 1);
    i->ref++;

/*     debug(__FILE__": ref++, now %i\n", i->ref); */
    pthread_mutex_unlock(&i->mutex);

    return i;
}

static void fd_info_unref(fd_info *i) {
    int r;
    pthread_mutex_lock(&i->mutex);
    assert(i->ref >= 1);
    r = --i->ref;
/*     debug(__FILE__": ref--, now %i\n", i->ref); */
    pthread_mutex_unlock(&i->mutex);

    if (r <= 0)
        fd_info_free(i);
}

static void context_state_cb(pa_context *c, void *userdata) {
    fd_info *i = userdata;
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
            pa_threaded_mainloop_signal(i->mainloop, 0);
            break;

        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
    }
}

static void reset_params(fd_info *i) {
    assert(i);
    
    i->sample_spec.format = PA_SAMPLE_ULAW;
    i->sample_spec.channels = 1;
    i->sample_spec.rate = 8000;
    i->fragment_size = 1024;
    i->n_fragments = 0;
}

static char *client_name(char *buf, size_t n) {
    char p[PATH_MAX];
    
    if (pa_get_binary_name(p, sizeof(p)))
        snprintf(buf, n, "oss[%s]", pa_path_get_filename(p));
    else
        snprintf(buf, n, "oss");

    return buf;
}

static fd_info* fd_info_new(fd_info_type_t type, int *_errno) {
    fd_info *i;
    int sfds[2] = { -1, -1 };
    char name[64];

    debug(__FILE__": fd_info_new()\n");

    signal(SIGPIPE, SIG_IGN); /* Yes, ugly as hell */
    
    if (!(i = malloc(sizeof(fd_info)))) {
        *_errno = ENOMEM;
        goto fail;
    }

    i->app_fd = i->thread_fd = -1;
    i->type = type;

    i->mainloop = NULL;
    i->context = NULL;
    i->stream = NULL;
    i->io_event = NULL;
    pthread_mutex_init(&i->mutex, NULL);
    i->ref = 1;
    i->buf = NULL;
    PA_LLIST_INIT(fd_info, i);

    reset_params(i);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sfds) < 0) {
        *_errno = errno;
        debug(__FILE__": socket() failed: %s\n", strerror(errno));
        goto fail;
    }

    i->app_fd = sfds[0];
    i->thread_fd = sfds[1];

    if (!(i->mainloop = pa_threaded_mainloop_new())) {
        *_errno = EIO;
        debug(__FILE__": pa_threaded_mainloop_new() failed\n");
        goto fail;
    }

    if (!(i->context = pa_context_new(pa_threaded_mainloop_get_api(i->mainloop), client_name(name, sizeof(name))))) {
        *_errno = EIO;
        debug(__FILE__": pa_context_new() failed\n");
        goto fail;
    }

    pa_context_set_state_callback(i->context, context_state_cb, i);

    if (pa_context_connect(i->context, NULL, 0, NULL) < 0) {
        *_errno = ECONNREFUSED;
        debug(__FILE__": pa_context_connect() failed: %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    pa_threaded_mainloop_lock(i->mainloop);

    if (pa_threaded_mainloop_start(i->mainloop) < 0) {
        *_errno = EIO;
        debug(__FILE__": pa_threaded_mainloop_start() failed\n");
        goto unlock_and_fail;
    }

    /* Wait until the context is ready */
    pa_threaded_mainloop_wait(i->mainloop);

    if (pa_context_get_state(i->context) != PA_CONTEXT_READY) {
        *_errno = ECONNREFUSED;
        debug(__FILE__": pa_context_connect() failed: %s\n", pa_strerror(pa_context_errno(i->context)));
        goto unlock_and_fail;
    }

    pa_threaded_mainloop_unlock(i->mainloop);
    return i;

unlock_and_fail:

    pa_threaded_mainloop_unlock(i->mainloop);
    
fail:

    if (i)
        fd_info_unref(i);
    
    return NULL;
}

static void fd_info_add_to_list(fd_info *i) {
    assert(i);

    pthread_mutex_lock(&fd_infos_mutex);
    PA_LLIST_PREPEND(fd_info, fd_infos, i);
    pthread_mutex_unlock(&fd_infos_mutex);

    fd_info_ref(i);
}

static void fd_info_remove_from_list(fd_info *i) {
    assert(i);

    pthread_mutex_lock(&fd_infos_mutex);
    PA_LLIST_REMOVE(fd_info, fd_infos, i);
    pthread_mutex_unlock(&fd_infos_mutex);

    fd_info_unref(i);
}

static fd_info* fd_info_find(int fd) {
    fd_info *i;

    pthread_mutex_lock(&fd_infos_mutex);
    
    for (i = fd_infos; i; i = i->next)
        if (i->app_fd == fd) {
            fd_info_ref(i);
            break;
        }

    pthread_mutex_unlock(&fd_infos_mutex);
    
    return i;
}

static void fix_metrics(fd_info *i) {
    size_t fs;
    char t[PA_SAMPLE_SPEC_SNPRINT_MAX];

    fs = pa_frame_size(&i->sample_spec);
    i->fragment_size = (i->fragment_size/fs)*fs;
    
    if (i->n_fragments < 2)
        i->n_fragments = 12;

    if (i->fragment_size <= 0)
        if ((i->fragment_size = pa_bytes_per_second(&i->sample_spec) / 2 / i->n_fragments) <= 0)
            i->fragment_size = 1024;

    debug(__FILE__": sample spec: %s\n", pa_sample_spec_snprint(t, sizeof(t), &i->sample_spec));
    debug(__FILE__": fixated metrics to %i fragments, %li bytes each.\n", i->n_fragments, (long)i->fragment_size);
}

static void stream_request_cb(pa_stream *s, size_t length, void *userdata) {
    fd_info *i = userdata;
    assert(s);

    if (i->io_event) {
        pa_mainloop_api *api;
        api = pa_threaded_mainloop_get_api(i->mainloop);
        api->io_enable(i->io_event, PA_IO_EVENT_INPUT);
    }
}

static void stream_latency_update_cb(pa_stream *s, void *userdata) {
    fd_info *i = userdata;
    assert(s);

    pa_threaded_mainloop_signal(i->mainloop, 0);
}

static void fd_info_shutdown(fd_info *i) {
    assert(i);

    if (i->io_event) {
        pa_mainloop_api *api;
        api = pa_threaded_mainloop_get_api(i->mainloop);
        api->io_free(i->io_event);
        i->io_event = NULL;
    }

    if (i->thread_fd >= 0) {
        close(i->thread_fd);
        i->thread_fd = -1;
    }
}

static int fd_info_copy_data(fd_info *i, int force) {
    size_t n;

    if (!i->stream)
        return -1;

    if ((n = pa_stream_writable_size(i->stream)) == (size_t) -1) {
        debug(__FILE__": pa_stream_writable_size(): %s\n", pa_strerror(pa_context_errno(i->context)));
        return -1;
    }
    
    while (n >= i->fragment_size || force) {
        ssize_t r;
        
        if (!i->buf) {
            if (!(i->buf = malloc(i->fragment_size))) {
                debug(__FILE__": malloc() failed.\n");
                return -1;
            }
        }
    
        if ((r = read(i->thread_fd, i->buf, i->fragment_size)) <= 0) {

            if (errno == EAGAIN)
                break;
            
            debug(__FILE__": read(): %s\n", r == 0 ? "EOF" : strerror(errno));
            return -1;
        }
    
        if (pa_stream_write(i->stream, i->buf, r, free, 0, PA_SEEK_RELATIVE) < 0) {
            debug(__FILE__": pa_stream_write(): %s\n", pa_strerror(pa_context_errno(i->context)));
            return -1;
        }

        i->buf = NULL;

        assert(n >= (size_t) r);
        n -= r;
    }

    if (i->io_event) {
        pa_mainloop_api *api;
        api = pa_threaded_mainloop_get_api(i->mainloop);
        api->io_enable(i->io_event, n >= i->fragment_size ? PA_IO_EVENT_INPUT : 0);
    }

    return 0;
}

static void stream_state_cb(pa_stream *s, void * userdata) {
    fd_info *i = userdata;
    assert(s);

    switch (pa_stream_get_state(s)) {

        case PA_STREAM_READY:
            debug(__FILE__": stream established.\n");
            break;
            
        case PA_STREAM_FAILED:
            debug(__FILE__": pa_stream_connect_playback() failed: %s\n", pa_strerror(pa_context_errno(i->context)));
            fd_info_shutdown(i);
            break;

        case PA_STREAM_TERMINATED:
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            break;
    }
}

static int create_stream(fd_info *i) {
    pa_buffer_attr attr;
    int n;
    
    assert(i);

    fix_metrics(i);

    if (!(i->stream = pa_stream_new(i->context, "audio stream", &i->sample_spec, NULL))) {
        debug(__FILE__": pa_stream_new() failed: %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    pa_stream_set_state_callback(i->stream, stream_state_cb, i);
    pa_stream_set_write_callback(i->stream, stream_request_cb, i);
    pa_stream_set_latency_update_callback(i->stream, stream_latency_update_cb, i);

    memset(&attr, 0, sizeof(attr));
    attr.maxlength = i->fragment_size * (i->n_fragments+1);
    attr.tlength = i->fragment_size * i->n_fragments;
    attr.prebuf = i->fragment_size;
    attr.minreq = i->fragment_size;
    
    if (pa_stream_connect_playback(i->stream, NULL, &attr, PA_STREAM_INTERPOLATE_TIMING|PA_STREAM_AUTO_TIMING_UPDATE, NULL, NULL) < 0) {
        debug(__FILE__": pa_stream_connect_playback() failed: %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    n = i->fragment_size;
    setsockopt(i->app_fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n));
    n = i->fragment_size;
    setsockopt(i->thread_fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
    
    return 0;

fail:
    return -1;
}

static void free_stream(fd_info *i) {
    assert(i);

    if (i->stream) {
        pa_stream_disconnect(i->stream);
        pa_stream_unref(i->stream);
        i->stream = NULL;
    }
}

static void io_event_cb(pa_mainloop_api *api, pa_io_event *e, int fd, pa_io_event_flags_t flags, void *userdata) {
    fd_info *i = userdata;

    pa_threaded_mainloop_signal(i->mainloop, 0);
    
    if (flags & PA_IO_EVENT_INPUT) {

        if (!i->stream) {
            api->io_enable(e, 0);

            if (create_stream(i) < 0)
                goto fail;

        } else {
            if (fd_info_copy_data(i, 0) < 0)
                goto fail;
        }
        
    } else if (flags & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR))
        goto fail;

    return;
    
fail:
    /* We can't do anything better than removing the event source */
    fd_info_shutdown(i);
}

static int dsp_open(int flags, int *_errno) {
    fd_info *i;
    pa_mainloop_api *api;
    int ret;
    int f;

    if ((flags != O_WRONLY) && (flags != (O_WRONLY|O_NONBLOCK))) {
        *_errno = EACCES;
        return -1;
    }
    
    if (!(i = fd_info_new(FD_INFO_PLAYBACK, _errno)))
        return -1;

    shutdown(i->thread_fd, SHUT_WR);
    shutdown(i->app_fd, SHUT_RD);

    if ((flags & O_NONBLOCK) == O_NONBLOCK) {
        if ((f = fcntl(i->app_fd, F_GETFL)) >= 0)
            fcntl(i->app_fd, F_SETFL, f|O_NONBLOCK);
    }
    if ((f = fcntl(i->thread_fd, F_GETFL)) >= 0)
        fcntl(i->thread_fd, F_SETFL, f|O_NONBLOCK);

    fcntl(i->app_fd, F_SETFD, FD_CLOEXEC);
    fcntl(i->thread_fd, F_SETFD, FD_CLOEXEC);

    pa_threaded_mainloop_lock(i->mainloop);
    api = pa_threaded_mainloop_get_api(i->mainloop);
    if (!(i->io_event = api->io_new(api, i->thread_fd, PA_IO_EVENT_INPUT, io_event_cb, i)))
        goto fail;
    
    pa_threaded_mainloop_unlock(i->mainloop);

    debug(__FILE__": dsp_open() succeeded, fd=%i\n", i->app_fd);

    fd_info_add_to_list(i);
    ret = i->app_fd;
    fd_info_unref(i);
    
    return ret;

fail:
    pa_threaded_mainloop_unlock(i->mainloop);

    if (i)
        fd_info_unref(i);
    
    *_errno = EIO;

    debug(__FILE__": dsp_open() failed\n");

    return -1;
}

static int mixer_open(int flags, int *_errno) {
/*     fd_info *i; */

    *_errno = ENOSYS;
    return -1;

/*     if (!(i = fd_info_new(FD_INFO_MIXER))) */
/*         return -1; */

}

int open(const char *filename, int flags, ...) {
    va_list args;
    mode_t mode = 0;
    int r, _errno = 0;

    va_start(args, flags);
    if (flags & O_CREAT)
        mode = va_arg(args, mode_t);
    va_end(args);

    if (!function_enter()) {
        LOAD_OPEN_FUNC();
        return _open(filename, flags, mode);
    }

    debug(__FILE__": open()\n");
    
    if (strcmp(filename, "/dev/dsp") == 0 || strcmp(filename, "/dev/adsp") == 0) {
        r = dsp_open(flags, &_errno);
    } else if (strcmp(filename, "/dev/mixer") == 0) {
        r = mixer_open(flags, &_errno);
    } else {
        function_exit();
        LOAD_OPEN_FUNC();
        return _open(filename, flags, mode);
    }

    function_exit();
    
    if (_errno)
        errno = _errno;
    
    return r;
}

static int mixer_ioctl(fd_info *i, unsigned long request, void*argp, int *_errno) {
    *_errno = ENOSYS;
    return -1;
}

static int map_format(int *fmt, pa_sample_spec *ss) {
    
    switch (*fmt) {
        case AFMT_MU_LAW:
            ss->format = PA_SAMPLE_ULAW;
            break;
            
        case AFMT_A_LAW:
            ss->format = PA_SAMPLE_ALAW;
            break;
            
        case AFMT_S8:
            *fmt = AFMT_U8;
            /* fall through */
        case AFMT_U8:
            ss->format = PA_SAMPLE_U8;
            break;
            
        case AFMT_U16_BE:
            *fmt = AFMT_S16_BE;
            /* fall through */
        case AFMT_S16_BE:
            ss->format = PA_SAMPLE_S16BE;
            break;
            
        case AFMT_U16_LE:
            *fmt = AFMT_S16_LE;
            /* fall through */
        case AFMT_S16_LE:
            ss->format = PA_SAMPLE_S16LE;
            break;
            
        default:
            ss->format = PA_SAMPLE_S16NE;
            *fmt = AFMT_S16_NE;
            break;
    }

    return 0;
}

static int map_format_back(pa_sample_format_t format) {
    switch (format) {
        case PA_SAMPLE_S16LE: return AFMT_S16_LE;
        case PA_SAMPLE_S16BE: return AFMT_S16_BE;
        case PA_SAMPLE_ULAW: return AFMT_MU_LAW;
        case PA_SAMPLE_ALAW: return AFMT_A_LAW;
        case PA_SAMPLE_U8: return AFMT_U8;
        default:
            abort();
    }
}

static void success_cb(pa_stream *s, int success, void *userdata) {
    fd_info *i = userdata;

    assert(s);
    assert(i);

    i->operation_success = success;
    pa_threaded_mainloop_signal(i->mainloop, 0);
}

static int dsp_empty_socket(fd_info *i) {
    int ret = -1;
    
    /* Empty the socket */
    for (;;) {
        int l;
        
        if (i->thread_fd < 0)
            break;
        
        if (ioctl(i->thread_fd, SIOCINQ, &l) < 0) {
            debug(__FILE__": SIOCINQ: %s\n", strerror(errno));
            break;
        }

        if (!l)
            break;
        
        pa_threaded_mainloop_wait(i->mainloop);
    }

    return ret;
}

static int dsp_drain(fd_info *i) {
    pa_operation *o = NULL;
    int r = -1;

    if (!i->mainloop)
        return 0;
    
    debug(__FILE__": Draining.\n");

    pa_threaded_mainloop_lock(i->mainloop);

    if (dsp_empty_socket(i) < 0)
        goto fail;
    
    if (!i->stream)
        goto fail;

    debug(__FILE__": Really draining.\n");
        
    if (!(o = pa_stream_drain(i->stream, success_cb, i))) {
        debug(__FILE__": pa_stream_drain(): %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    i->operation_success = 0;
    while (pa_operation_get_state(o) != PA_OPERATION_DONE) {
        if (!i->stream || pa_stream_get_state(i->stream) != PA_STREAM_READY)
            goto fail;
            
        pa_threaded_mainloop_wait(i->mainloop);
    }

    if (!i->operation_success) {
        debug(__FILE__": pa_stream_drain() 2: %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    r = 0;
    
fail:
    
    if (o)
        pa_operation_unref(o);

    pa_threaded_mainloop_unlock(i->mainloop);

    return 0;
}

static int dsp_trigger(fd_info *i) {
    pa_operation *o = NULL;
    int r = -1;

    fd_info_copy_data(i, 1);

    if (!i->stream)
        return 0;

    pa_threaded_mainloop_lock(i->mainloop);

    if (dsp_empty_socket(i) < 0)
        goto fail;

    debug(__FILE__": Triggering.\n");
        
    if (!(o = pa_stream_trigger(i->stream, success_cb, i))) {
        debug(__FILE__": pa_stream_trigger(): %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    i->operation_success = 0;
    while (!pa_operation_get_state(o) != PA_OPERATION_DONE) {
        if (!i->stream || pa_stream_get_state(i->stream) != PA_STREAM_READY)
            goto fail;
            
        pa_threaded_mainloop_wait(i->mainloop);
    }

    if (!i->operation_success) {
        debug(__FILE__": pa_stream_trigger(): %s\n", pa_strerror(pa_context_errno(i->context)));
        goto fail;
    }

    r = 0;
    
fail:
    
    if (o)
        pa_operation_unref(o);

    pa_threaded_mainloop_unlock(i->mainloop);

    return 0;
}

static int dsp_ioctl(fd_info *i, unsigned long request, void*argp, int *_errno) {
    int ret = -1;
    
    switch (request) {
        case SNDCTL_DSP_SETFMT: {
            debug(__FILE__": SNDCTL_DSP_SETFMT: %i\n", *(int*) argp);
            
            pa_threaded_mainloop_lock(i->mainloop);

            if (*(int*) argp == AFMT_QUERY)
                *(int*) argp = map_format_back(i->sample_spec.format);
            else {
                map_format((int*) argp, &i->sample_spec);
                free_stream(i);
            }

            pa_threaded_mainloop_unlock(i->mainloop);
            break;
        }
            
        case SNDCTL_DSP_SPEED: {
            pa_sample_spec ss;
            int valid;
            
            debug(__FILE__": SNDCTL_DSP_SPEED: %i\n", *(int*) argp);

            pa_threaded_mainloop_lock(i->mainloop);

            ss = i->sample_spec;
            ss.rate = *(int*) argp;

            if ((valid = pa_sample_spec_valid(&ss))) {
                i->sample_spec = ss;
                free_stream(i);
            }
            
            pa_threaded_mainloop_unlock(i->mainloop);

            if (!valid) {
                *_errno = EINVAL;
                goto fail;
            }
            
            break;
        }
            
        case SNDCTL_DSP_STEREO:
            debug(__FILE__": SNDCTL_DSP_STEREO: %i\n", *(int*) argp);
            
            pa_threaded_mainloop_lock(i->mainloop);
            
            i->sample_spec.channels = *(int*) argp ? 2 : 1;
            free_stream(i);
            
            pa_threaded_mainloop_unlock(i->mainloop);
            return 0;

        case SNDCTL_DSP_CHANNELS: {
            pa_sample_spec ss;
            int valid;
            
            debug(__FILE__": SNDCTL_DSP_CHANNELS: %i\n", *(int*) argp);
            
            pa_threaded_mainloop_lock(i->mainloop);

            ss = i->sample_spec;
            ss.channels = *(int*) argp;

            if ((valid = pa_sample_spec_valid(&ss))) {
                i->sample_spec = ss;
                free_stream(i);
            }
            
            pa_threaded_mainloop_unlock(i->mainloop);

            if (!valid) {
                *_errno = EINVAL;
                goto fail;
            }

            break;
        }

        case SNDCTL_DSP_GETBLKSIZE:
            debug(__FILE__": SNDCTL_DSP_GETBLKSIZE\n");

            pa_threaded_mainloop_lock(i->mainloop);

            fix_metrics(i);
            *(int*) argp = i->fragment_size;
            
            pa_threaded_mainloop_unlock(i->mainloop);
            
            break;

        case SNDCTL_DSP_SETFRAGMENT:
            debug(__FILE__": SNDCTL_DSP_SETFRAGMENT: 0x%8x\n", *(int*) argp);
            
            pa_threaded_mainloop_lock(i->mainloop);
            
            i->fragment_size = 1 << (*(int*) argp);
            i->n_fragments = (*(int*) argp) >> 16;
            
            free_stream(i);
            
            pa_threaded_mainloop_unlock(i->mainloop);
            
            break;
            
        case SNDCTL_DSP_GETCAPS:
            debug(__FILE__": SNDCTL_DSP_CAPS\n");
            
            *(int*)  argp = DSP_CAP_MULTI;
            break;

        case SNDCTL_DSP_GETODELAY: {
            int l;
            
            debug(__FILE__": SNDCTL_DSP_GETODELAY\n");
            
            pa_threaded_mainloop_lock(i->mainloop);

            *(int*) argp = 0;
            
            for (;;) {
                pa_usec_t usec;
                if (!i->stream || pa_stream_get_state(i->stream) != PA_STREAM_READY)
                    break;

                if (pa_stream_get_latency(i->stream, &usec, NULL) >= 0) {
                    *(int*) argp = pa_usec_to_bytes(usec, &i->sample_spec);
                    break;
                }

                if (pa_context_errno(i->context) != PA_ERR_NODATA) {
                    debug(__FILE__": pa_stream_get_latency(): %s\n", pa_strerror(pa_context_errno(i->context)));
                    break;
                }

                pa_threaded_mainloop_wait(i->mainloop);
            }
            
            if (ioctl(i->thread_fd, SIOCINQ, &l) < 0)
                debug(__FILE__": SIOCINQ failed: %s\n", strerror(errno));
            else
                *(int*) argp += l;

            pa_threaded_mainloop_unlock(i->mainloop);

            debug(__FILE__": ODELAY: %i\n", *(int*) argp);

            break;
        }
            
        case SNDCTL_DSP_RESET: {
            debug(__FILE__": SNDCTL_DSP_RESET\n");
            
            pa_threaded_mainloop_lock(i->mainloop);

            free_stream(i);
            reset_params(i);
            
            pa_threaded_mainloop_unlock(i->mainloop);
            break;
        }
            
        case SNDCTL_DSP_GETFMTS: {
            debug(__FILE__": SNDCTL_DSP_GETFMTS\n");
            
            *(int*) argp = AFMT_MU_LAW|AFMT_A_LAW|AFMT_U8|AFMT_S16_LE|AFMT_S16_BE;
            break;
        }

        case SNDCTL_DSP_POST:
            debug(__FILE__": SNDCTL_DSP_POST\n");
            
            if (dsp_trigger(i) < 0) 
                *_errno = EIO;
            break;

        case SNDCTL_DSP_SYNC: 
            debug(__FILE__": SNDCTL_DSP_SYNC\n");
            
            if (dsp_drain(i) < 0) 
                *_errno = EIO;

            break;

        case SNDCTL_DSP_GETOSPACE: {
            audio_buf_info *bi = (audio_buf_info*) argp;
            int l;
            size_t k = 0;

            debug(__FILE__": SNDCTL_DSP_GETOSPACE\n");

            pa_threaded_mainloop_lock(i->mainloop);

            fix_metrics(i);
            
            if (i->stream) {
                if ((k = pa_stream_writable_size(i->stream)) == (size_t) -1)
                    debug(__FILE__": pa_stream_writable_size(): %s\n", pa_strerror(pa_context_errno(i->context)));
            } else
                k = i->fragment_size * i->n_fragments;
            
            if (ioctl(i->thread_fd, SIOCINQ, &l) < 0) {
                debug(__FILE__": SIOCINQ failed: %s\n", strerror(errno));
                l = 0;
            }

            bi->fragsize = i->fragment_size;
            bi->fragstotal = i->n_fragments;
            bi->bytes = k > (size_t) l ? k - l : 0;
            bi->fragments = bi->bytes / bi->fragsize;

            pa_threaded_mainloop_unlock(i->mainloop);

            debug(__FILE__": fragsize=%i, fragstotal=%i, bytes=%i, fragments=%i\n", bi->fragsize, bi->fragstotal, bi->bytes, bi->fragments);

            break;
        }
            
        default:
            debug(__FILE__": unknwon ioctl 0x%08lx\n", request);

            *_errno = EINVAL;
            goto fail;
    }

    ret = 0;
    
fail:
    
    return ret;
}

int ioctl(int fd, unsigned long request, ...) {
    fd_info *i;
    va_list args;
    void *argp;
    int r, _errno = 0;

    debug(__FILE__": ioctl()\n");

    va_start(args, request);
    argp = va_arg(args, void *);
    va_end(args);

    if (!function_enter()) {
        LOAD_IOCTL_FUNC();
        return _ioctl(fd, request, argp);
    }

    if (!(i = fd_info_find(fd))) {
        function_exit();
        LOAD_IOCTL_FUNC();
        return _ioctl(fd, request, argp);
    }

    if (i->type == FD_INFO_MIXER)
        r = mixer_ioctl(i, request, argp, &_errno);
    else
        r = dsp_ioctl(i, request, argp, &_errno);
    
    fd_info_unref(i);

    if (_errno)
        errno = _errno;

    function_exit();
    
    return r;
}

int close(int fd) {
    fd_info *i;

    debug(__FILE__": close()\n");

    if (!function_enter()) {
        LOAD_CLOSE_FUNC();
        return _close(fd);
    }

    if (!(i = fd_info_find(fd))) {
        function_exit();
        LOAD_CLOSE_FUNC();
        return _close(fd);
    }

    fd_info_remove_from_list(i);
    fd_info_unref(i);
    
    function_exit();

    return 0;
}

int open64(const char *filename, int flags, ...) {
    va_list args;
    mode_t mode = 0;

    debug(__FILE__": open64()\n");
    
    va_start(args, flags);
    if (flags & O_CREAT)
        mode = va_arg(args, mode_t);
    va_end(args);

    if (strcmp(filename, "/dev/dsp") != 0 &&
        strcmp(filename, "/dev/adsp") != 0 &&
        strcmp(filename, "/dev/mixer") != 0) {
        LOAD_OPEN64_FUNC();
        return _open64(filename, flags, mode);
    }

    return open(filename, flags, mode);
}

FILE* fopen(const char *filename, const char *mode) {
    FILE *f = NULL;
    int fd;

    debug(__FILE__": fopen()\n");

    if (strcmp(filename, "/dev/dsp") != 0 &&
        strcmp(filename, "/dev/adsp") != 0 &&
        strcmp(filename, "/dev/mixer") != 0) {
        LOAD_FOPEN_FUNC();
        return _fopen(filename, mode);
    }

    if (strcmp(mode, "wb") != 0) {
        errno = EACCES;
        return NULL;
    }

    if ((fd = open(filename, O_WRONLY)) < 0)
        return NULL;

    if (!(f = fdopen(fd, "wb"))) {
        close(fd);
        return NULL;
    }
    
    return f;
}

FILE *fopen64(const char *filename, const char *mode) {

    debug(__FILE__": fopen64()\n");

    if (strcmp(filename, "/dev/dsp") != 0 &&
        strcmp(filename, "/dev/adsp") != 0 &&
        strcmp(filename, "/dev/mixer") != 0) {
        LOAD_FOPEN64_FUNC();
        return _fopen64(filename, mode);
    }

    return fopen(filename, mode);
}

int fclose(FILE *f) {
    fd_info *i;

    debug(__FILE__": fclose()\n");

    if (!function_enter()) {
        LOAD_FCLOSE_FUNC();
        return _fclose(f);
    }

    if (!(i = fd_info_find(fileno(f)))) {
        function_exit();
        LOAD_FCLOSE_FUNC();
        return _fclose(f);
    }

    fd_info_remove_from_list(i);

    /* Dirty trick to avoid that the fd is not freed twice, once by us
     * and once by the real fclose() */
    i->app_fd = -1;
    
    fd_info_unref(i);
    
    function_exit();

    LOAD_FCLOSE_FUNC();
    return _fclose(f);
}
