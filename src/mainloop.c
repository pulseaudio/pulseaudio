#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/poll.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#include "mainloop.h"
#include "util.h"
#include "idxset.h"

struct mainloop_source_header {
    struct pa_mainloop *mainloop;
    int dead;
};
    
struct mainloop_source_io {
    struct mainloop_source_header header;
    
    int fd;
    enum pa_mainloop_api_io_events events;
    void (*callback) (struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata);
    void *userdata;

    struct pollfd *pollfd;
};

struct mainloop_source_fixed_or_idle {
    struct mainloop_source_header header;
    int enabled;

    void (*callback)(struct pa_mainloop_api*a, void *id, void *userdata);
    void *userdata;
};

struct mainloop_source_time {
    struct mainloop_source_header header;
    int enabled;
    
    struct timeval timeval;
    void (*callback)(struct pa_mainloop_api*a, void *id, const struct timeval*tv, void *userdata);
    void *userdata;
};

struct pa_mainloop {
    struct pa_idxset *io_sources, *fixed_sources, *idle_sources, *time_sources;
    int io_sources_scan_dead, fixed_sources_scan_dead, idle_sources_scan_dead, time_sources_scan_dead;

    struct pollfd *pollfds;
    unsigned max_pollfds, n_pollfds;
    int rebuild_pollfds;

    int quit, running, retval;
    struct pa_mainloop_api api;
};

static void setup_api(struct pa_mainloop *m);

struct pa_mainloop *pa_mainloop_new(void) {
    struct pa_mainloop *m;

    m = malloc(sizeof(struct pa_mainloop));
    assert(m);

    m->io_sources = pa_idxset_new(NULL, NULL);
    m->fixed_sources = pa_idxset_new(NULL, NULL);
    m->idle_sources = pa_idxset_new(NULL, NULL);
    m->time_sources = pa_idxset_new(NULL, NULL);

    assert(m->io_sources && m->fixed_sources && m->idle_sources && m->time_sources);

    m->io_sources_scan_dead = m->fixed_sources_scan_dead = m->idle_sources_scan_dead = m->time_sources_scan_dead = 0;
    
    m->pollfds = NULL;
    m->max_pollfds = m->n_pollfds = m->rebuild_pollfds = 0;

    m->quit = m->running = m->retval = 0;

    setup_api(m);
    
    return m;
}

static int foreach(void *p, uint32_t index, int *del, void*userdata) {
    struct mainloop_source_header *h = p;
    int *all = userdata;
    assert(p && del && all);

    if (*all || h->dead) {
        free(h);
        *del = 1;
    }

    return 0;
};

void pa_mainloop_free(struct pa_mainloop* m) {
    int all = 1;
    assert(m);
    pa_idxset_foreach(m->io_sources, foreach, &all);
    pa_idxset_foreach(m->fixed_sources, foreach, &all);
    pa_idxset_foreach(m->idle_sources, foreach, &all);
    pa_idxset_foreach(m->time_sources, foreach, &all);

    pa_idxset_free(m->io_sources, NULL, NULL);
    pa_idxset_free(m->fixed_sources, NULL, NULL);
    pa_idxset_free(m->idle_sources, NULL, NULL);
    pa_idxset_free(m->time_sources, NULL, NULL);

    free(m->pollfds);
    free(m);
}

static void scan_dead(struct pa_mainloop *m) {
    int all = 0;
    assert(m);
    if (m->io_sources_scan_dead)
        pa_idxset_foreach(m->io_sources, foreach, &all);
    if (m->fixed_sources_scan_dead)
        pa_idxset_foreach(m->fixed_sources, foreach, &all);
    if (m->idle_sources_scan_dead)
        pa_idxset_foreach(m->idle_sources, foreach, &all);
    if (m->time_sources_scan_dead)
        pa_idxset_foreach(m->time_sources, foreach, &all);
}

static void rebuild_pollfds(struct pa_mainloop *m) {
    struct mainloop_source_io*s;
    struct pollfd *p;
    uint32_t index = PA_IDXSET_INVALID;
    unsigned l;

    l = pa_idxset_ncontents(m->io_sources);
    if (m->max_pollfds < l) {
        m->pollfds = realloc(m->pollfds, sizeof(struct pollfd)*l);
        m->max_pollfds = l;
    }

    m->n_pollfds = 0;
    p = m->pollfds;
    for (s = pa_idxset_first(m->io_sources, &index); s; s = pa_idxset_next(m->io_sources, &index)) {
        if (s->header.dead) {
            s->pollfd = NULL;
            continue;
        }

        s->pollfd = p;
        p->fd = s->fd;
        p->events = ((s->events & PA_MAINLOOP_API_IO_EVENT_INPUT) ? POLLIN : 0) | ((s->events & PA_MAINLOOP_API_IO_EVENT_OUTPUT) ? POLLOUT : 0);
        p->revents = 0;

        p++;
        m->n_pollfds++;
    }
}

static void dispatch_pollfds(struct pa_mainloop *m) {
    uint32_t index = PA_IDXSET_INVALID;
    struct mainloop_source_io *s;

    for (s = pa_idxset_first(m->io_sources, &index); s; s = pa_idxset_next(m->io_sources, &index)) {
        if (s->header.dead || !s->pollfd || !s->pollfd->revents)
            continue;
        
        assert(s->pollfd->fd == s->fd && s->callback);
        s->callback(&m->api, s, s->fd,
                    ((s->pollfd->revents & (POLLIN|POLLHUP|POLLERR)) ? PA_MAINLOOP_API_IO_EVENT_INPUT : 0) |
                    ((s->pollfd->revents & POLLOUT) ? PA_MAINLOOP_API_IO_EVENT_OUTPUT : 0), s->userdata);
        s->pollfd->revents = 0;
    }
}

static void run_fixed_or_idle(struct pa_mainloop *m, struct pa_idxset *i) {
    uint32_t index = PA_IDXSET_INVALID;
    struct mainloop_source_fixed_or_idle *s;

    for (s = pa_idxset_first(i, &index); s; s = pa_idxset_next(i, &index)) {
        if (s->header.dead || !s->enabled)
            continue;

        assert(s->callback);
        s->callback(&m->api, s, s->userdata);
    }
}

static int calc_next_timeout(struct pa_mainloop *m) {
    uint32_t index = PA_IDXSET_INVALID;
    struct mainloop_source_time *s;
    struct timeval now;
    int t = -1;

    if (pa_idxset_isempty(m->time_sources))
        return -1;

    gettimeofday(&now, NULL);
    
    for (s = pa_idxset_first(m->time_sources, &index); s; s = pa_idxset_next(m->time_sources, &index)) {
        int tmp;
        
        if (s->header.dead || !s->enabled)
            continue;

        if (s->timeval.tv_sec < now.tv_sec || (s->timeval.tv_sec == now.tv_sec && s->timeval.tv_usec <= now.tv_usec)) 
            return 0;

        tmp = (s->timeval.tv_sec - now.tv_sec)*1000;
            
        if (s->timeval.tv_usec > now.tv_usec)
            tmp += (s->timeval.tv_usec - now.tv_usec)/1000;
        else
            tmp -= (now.tv_usec - s->timeval.tv_usec)/1000;

        if (tmp == 0)
            return 0;
        else if (t == -1 || tmp < t)
            t = tmp;
    }

    return t;
}

static void dispatch_timeout(struct pa_mainloop *m) {
    uint32_t index = PA_IDXSET_INVALID;
    struct mainloop_source_time *s;
    struct timeval now;
    assert(m);

    if (pa_idxset_isempty(m->time_sources))
        return;

    gettimeofday(&now, NULL);
    for (s = pa_idxset_first(m->time_sources, &index); s; s = pa_idxset_next(m->time_sources, &index)) {
        
        if (s->header.dead || !s->enabled)
            continue;

        if (s->timeval.tv_sec < now.tv_sec || (s->timeval.tv_sec == now.tv_sec && s->timeval.tv_usec <= now.tv_usec)) {
            assert(s->callback);

            s->enabled = 0;
            s->callback(&m->api, s, &s->timeval, s->userdata);
        }
    }
}

static int any_idle_sources(struct pa_mainloop *m) {
    struct mainloop_source_fixed_or_idle *s;
    uint32_t index;
    assert(m);
    
    for (s = pa_idxset_first(m->idle_sources, &index); s; s = pa_idxset_next(m->idle_sources, &index))
        if (!s->header.dead && s->enabled)
            return 1;

    return 0;
}

int pa_mainloop_iterate(struct pa_mainloop *m, int block, int *retval) {
    int r, idle;
    assert(m && !m->running);
    
    if(m->quit) {
        if (retval)
            *retval = m->retval;
        return 1;
    }

    m->running = 1;

    scan_dead(m);
    run_fixed_or_idle(m, m->fixed_sources);

    if (m->rebuild_pollfds) {
        rebuild_pollfds(m);
        m->rebuild_pollfds = 0;
    }

    idle = any_idle_sources(m);

    do {
        int t;

        if (!block || idle)
            t = 0;
        else 
            t = calc_next_timeout(m);
            
        r = poll(m->pollfds, m->n_pollfds, t);
    } while (r < 0 && errno == EINTR);

    dispatch_timeout(m);
    
    if (r > 0)
        dispatch_pollfds(m);
    else if (r == 0 && idle)
        run_fixed_or_idle(m, m->idle_sources);
    else if (r < 0)
        fprintf(stderr, "select(): %s\n", strerror(errno));
    
    m->running = 0;
    return r < 0 ? -1 : 0;
}

int pa_mainloop_run(struct pa_mainloop *m, int *retval) {
    int r;
    while ((r = pa_mainloop_iterate(m, 1, retval)) == 0);
    return r;
}

void pa_mainloop_quit(struct pa_mainloop *m, int r) {
    assert(m);
    m->quit = r;
}

/* IO sources */
static void* mainloop_source_io(struct pa_mainloop_api*a, int fd, enum pa_mainloop_api_io_events events, void (*callback) (struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct mainloop_source_io *s;
    assert(a && a->userdata && fd >= 0 && callback);
    m = a->userdata;
    assert(a == &m->api);

    s = malloc(sizeof(struct mainloop_source_io));
    assert(s);
    s->header.mainloop = m;
    s->header.dead = 0;

    s->fd = fd;
    s->events = events;
    s->callback = callback;
    s->userdata = userdata;
    s->pollfd = NULL;

    pa_idxset_put(m->io_sources, s, NULL);
    m->rebuild_pollfds = 1;
    return s;
}

static void mainloop_enable_io(struct pa_mainloop_api*a, void* id, enum pa_mainloop_api_io_events events) {
    struct pa_mainloop *m;
    struct mainloop_source_io *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api && s->header.mainloop == m);

    s->events = events;
    if (s->pollfd)
        s->pollfd->events = ((s->events & PA_MAINLOOP_API_IO_EVENT_INPUT) ? POLLIN : 0) | ((s->events & PA_MAINLOOP_API_IO_EVENT_OUTPUT) ? POLLOUT : 0);
}

static void mainloop_cancel_io(struct pa_mainloop_api*a, void* id) {
    struct pa_mainloop *m;
    struct mainloop_source_io *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api && s->header.mainloop == m);

    s->header.dead = 1;
    m->io_sources_scan_dead = 1;
    m->rebuild_pollfds = 1;
}

/* Fixed sources */
static void* mainloop_source_fixed(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, void *id, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct mainloop_source_fixed_or_idle *s;
    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    s = malloc(sizeof(struct mainloop_source_fixed_or_idle));
    assert(s);
    s->header.mainloop = m;
    s->header.dead = 0;

    s->enabled = 1;
    s->callback = callback;
    s->userdata = userdata;

    pa_idxset_put(m->fixed_sources, s, NULL);
    return s;
}

static void mainloop_enable_fixed(struct pa_mainloop_api*a, void* id, int b) {
    struct pa_mainloop *m;
    struct mainloop_source_fixed_or_idle *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api);

    s->enabled = b;
}

static void mainloop_cancel_fixed(struct pa_mainloop_api*a, void* id) {
    struct pa_mainloop *m;
    struct mainloop_source_fixed_or_idle *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api);

    s->header.dead = 1;
    m->fixed_sources_scan_dead = 1;
}

/* Idle sources */
static void* mainloop_source_idle(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, void *id, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct mainloop_source_fixed_or_idle *s;
    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    s = malloc(sizeof(struct mainloop_source_fixed_or_idle));
    assert(s);
    s->header.mainloop = m;
    s->header.dead = 0;

    s->enabled = 1;
    s->callback = callback;
    s->userdata = userdata;

    pa_idxset_put(m->idle_sources, s, NULL);
    return s;
}

static void mainloop_cancel_idle(struct pa_mainloop_api*a, void* id) {
    struct pa_mainloop *m;
    struct mainloop_source_fixed_or_idle *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api);

    s->header.dead = 1;
    m->idle_sources_scan_dead = 1;
}

/* Time sources */
static void* mainloop_source_time(struct pa_mainloop_api*a, const struct timeval *tv, void (*callback) (struct pa_mainloop_api*a, void *id, const struct timeval *tv, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct mainloop_source_time *s;
    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    s = malloc(sizeof(struct mainloop_source_time));
    assert(s);
    s->header.mainloop = m;
    s->header.dead = 0;

    s->enabled = !!tv;
    if (tv)
        s->timeval = *tv;

    s->callback = callback;
    s->userdata = userdata;

    pa_idxset_put(m->time_sources, s, NULL);
    return s;
}

static void mainloop_enable_time(struct pa_mainloop_api*a, void *id, const struct timeval *tv) {
    struct pa_mainloop *m;
    struct mainloop_source_time *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api);

    if (tv) {
        s->enabled = 1;
        s->timeval = *tv;
    } else
        s->enabled = 0;
}

static void mainloop_cancel_time(struct pa_mainloop_api*a, void* id) {
    struct pa_mainloop *m;
    struct mainloop_source_time *s = id;
    assert(a && a->userdata && s && !s->header.dead);
    m = a->userdata;
    assert(a == &m->api);

    s->header.dead = 1;
    m->time_sources_scan_dead = 1;

}

static void mainloop_quit(struct pa_mainloop_api*a, int retval) {
    struct pa_mainloop *m;
    assert(a && a->userdata);
    m = a->userdata;
    assert(a == &m->api);

    m->quit = 1;
    m->retval = retval;
}
    
static void setup_api(struct pa_mainloop *m) {
    assert(m);
    
    m->api.userdata = m;
    m->api.source_io = mainloop_source_io;
    m->api.enable_io = mainloop_enable_io;
    m->api.cancel_io = mainloop_cancel_io;

    m->api.source_fixed = mainloop_source_fixed;
    m->api.enable_fixed = mainloop_enable_fixed;
    m->api.cancel_fixed = mainloop_cancel_fixed;

    m->api.source_idle = mainloop_source_idle;
    m->api.enable_idle = mainloop_enable_fixed; /* (!) */
    m->api.cancel_idle = mainloop_cancel_idle;
    
    m->api.source_time = mainloop_source_time;
    m->api.enable_time = mainloop_enable_time;
    m->api.cancel_time = mainloop_cancel_time;

    m->api.quit = mainloop_quit;
}

struct pa_mainloop_api* pa_mainloop_get_api(struct pa_mainloop*m) {
    assert(m);
    return &m->api;
}
        
