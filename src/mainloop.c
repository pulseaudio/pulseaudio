#include <signal.h>
#include <unistd.h>
#include <sys/poll.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#include "mainloop.h"

struct mainloop_source {
    struct mainloop_source *next;
    struct mainloop *mainloop;
    enum mainloop_source_type type;

    int enabled;
    int dead;
    void *userdata;

    struct {
        int fd;
        enum mainloop_io_event events;
        void (*callback)(struct mainloop_source*s, int fd, enum mainloop_io_event event, void *userdata);
        struct pollfd pollfd;
    } io;
    
    struct  {
        void (*callback)(struct mainloop_source*s, void *userdata);
    } prepare;
    
    struct  {
        void (*callback)(struct mainloop_source*s, void *userdata);
    } idle;

    struct {
        int sig;
        struct sigaction sigaction;
        void (*callback)(struct mainloop_source*s, int sig, void *userdata);
    } signal;
};

struct mainloop_source_list {
    struct mainloop_source *sources;
    int n_sources;
    int dead_sources;
};

struct mainloop {
    struct mainloop_source_list io_sources, prepare_sources, idle_sources, signal_sources;
    
    struct pollfd *pollfds;
    int max_pollfds, n_pollfds;
    int rebuild_pollfds;

    int quit;
    int running;
    int signal_pipe[2];
    struct pollfd signal_pollfd;
};

static int signal_pipe = -1;

static void signal_func(int sig) {
    if (signal_pipe >= 0)
        write(signal_pipe, &sig, sizeof(sig));
}

static void make_nonblock(int fd) {
    int v;
    
    if ((v = fcntl(fd, F_GETFL)) >= 0)
        fcntl(fd, F_SETFL, v|O_NONBLOCK);
}


struct mainloop *mainloop_new(void) {
    int r;
    struct mainloop *m;

    m = malloc(sizeof(struct mainloop));
    assert(m);
    memset(m, 0, sizeof(struct mainloop));

    r = pipe(m->signal_pipe);
    assert(r >= 0 && m->signal_pipe[0] >= 0 && m->signal_pipe[1] >= 0);

    make_nonblock(m->signal_pipe[0]);
    make_nonblock(m->signal_pipe[1]);
    
    signal_pipe = m->signal_pipe[1];
    m->signal_pollfd.fd = m->signal_pipe[0];
    m->signal_pollfd.events = POLLIN;
    m->signal_pollfd.revents = 0;
    
    return m;
}

static void free_sources(struct mainloop_source_list *l, int all) {
    struct mainloop_source *s, *p;
    assert(l);

    if (!all && !l->dead_sources)
        return;

    p = NULL;
    s = l->sources;
    while (s) {
        if (all || s->dead) {
            struct mainloop_source *t = s;
            s = s->next;

            if (p)
                p->next = s;
            else
                l->sources = s;
            
            free(t);
        } else {
            p = s;
            s = s->next;
        }
    }

    l->dead_sources = 0;

    if (all) {
        assert(!l->sources);
        l->n_sources = 0;
    }
}

void mainloop_free(struct mainloop* m) {
    assert(m);
    free_sources(&m->io_sources, 1);
    free_sources(&m->prepare_sources, 1);
    free_sources(&m->idle_sources, 1);
    free_sources(&m->signal_sources, 1);

    if (signal_pipe == m->signal_pipe[1])
        signal_pipe = -1;
    close(m->signal_pipe[0]);
    close(m->signal_pipe[1]);
    
    free(m->pollfds);
    free(m);
}

static void rebuild_pollfds(struct mainloop *m) {
    struct mainloop_source*s;
    struct pollfd *p;
    
    if (m->max_pollfds < m->io_sources.n_sources+1) {
        m->max_pollfds = (m->io_sources.n_sources+1)*2;
        m->pollfds = realloc(m->pollfds, sizeof(struct pollfd)*m->max_pollfds);
    }

    m->n_pollfds = 0;
    p = m->pollfds;
    for (s = m->io_sources.sources; s; s = s->next) {
        assert(s->type == MAINLOOP_SOURCE_TYPE_IO);
        if (!s->dead && s->enabled && s->io.events != MAINLOOP_IO_EVENT_NULL) {
            *(p++) = s->io.pollfd;
            m->n_pollfds++;
        }
    }

    *(p++) = m->signal_pollfd;
    m->n_pollfds++;
}

static void dispatch_pollfds(struct mainloop *m) {
    int i;
    struct pollfd *p;
    struct mainloop_source *s;
    /* This loop assumes that m->sources and m->pollfds have the same
     * order and that m->pollfds is a subset of m->sources! */

    s = m->io_sources.sources;
    for (p = m->pollfds, i = 0; i < m->n_pollfds; p++, i++) {
        if (!p->revents)
            continue;

        if (p->fd == m->signal_pipe[0]) {
            /* Event from signal pipe */

            if (p->revents & POLLIN) {
                int sig;
                ssize_t r;
                r = read(m->signal_pipe[0], &sig, sizeof(sig));
                assert((r < 0 && errno == EAGAIN) || r == sizeof(sig));
            
                if (r == sizeof(sig)) {
                    struct mainloop_source *l = m->signal_sources.sources;
                    while (l) {
                        assert(l->type == MAINLOOP_SOURCE_TYPE_SIGNAL);
                        
                        if (l->signal.sig == sig && l->enabled && !l->dead) {
                            assert(l->signal.callback);
                            l->signal.callback(l, sig, l->userdata);
                        }
                        
                        l = l->next;
                    }
                }
            }

        } else {
            /* Event from I/O source */

            for (; s; s = s->next) {
                if (p->fd != s->io.fd)
                    continue;
                
                assert(s->type == MAINLOOP_SOURCE_TYPE_IO);

                if (!s->dead && s->enabled) {
                    enum mainloop_io_event e = (p->revents & POLLIN ? MAINLOOP_IO_EVENT_IN : 0) | (p->revents & POLLOUT ? MAINLOOP_IO_EVENT_OUT : 0);
                    if (e) {
                        assert(s->io.callback);
                        s->io.callback(s, s->io.fd, e, s->userdata);
                    }
                }

                break;
            }
        }
    }
}

int mainloop_iterate(struct mainloop *m, int block) {
    struct mainloop_source *s;
    int c;
    assert(m && !m->running);
    
    if(m->quit)
        return m->quit;

    free_sources(&m->io_sources, 0);
    free_sources(&m->prepare_sources, 0);
    free_sources(&m->idle_sources, 0);

    for (s = m->prepare_sources.sources; s; s = s->next) {
        assert(!s->dead && s->type == MAINLOOP_SOURCE_TYPE_PREPARE);
        if (s->enabled) {
            assert(s->prepare.callback);
            s->prepare.callback(s, s->userdata);
        }   
    }

    if (m->rebuild_pollfds)
        rebuild_pollfds(m);

    m->running = 1;

    do {
        c = poll(m->pollfds, m->n_pollfds, (block && !m->idle_sources.n_sources) ? -1 : 0);
    } while (c < 0 && errno == EINTR);
        
    if (c > 0)
        dispatch_pollfds(m);
    else if (c == 0) {
        for (s = m->idle_sources.sources; s; s = s->next) {
            assert(!s->dead && s->type == MAINLOOP_SOURCE_TYPE_IDLE);
            if (s->enabled) {
                assert(s->idle.callback);
                s->idle.callback(s, s->userdata);
            }
        }
    }
    
    m->running = 0;
    return c < 0 ? -1 : 0;
}

int mainloop_run(struct mainloop *m) {
    int r;
    while (!(r = mainloop_iterate(m, 1)));
    return r;
}

void mainloop_quit(struct mainloop *m, int r) {
    assert(m);
    m->quit = r;
}

static struct mainloop_source_list* get_source_list(struct mainloop *m, enum mainloop_source_type type) {
    struct mainloop_source_list *l;
    
    switch(type) {
        case MAINLOOP_SOURCE_TYPE_IO:
            l = &m->io_sources;
            break;
        case MAINLOOP_SOURCE_TYPE_PREPARE:
            l = &m->prepare_sources;
            break;
        case MAINLOOP_SOURCE_TYPE_IDLE:
            l = &m->idle_sources;
            break;
        case MAINLOOP_SOURCE_TYPE_SIGNAL:
            l = &m->signal_sources;
            break;
        default:
            l = NULL;
            break;
    }
    
    return l;
}

static struct mainloop_source *source_new(struct mainloop*m, enum mainloop_source_type type) {
    struct mainloop_source_list *l;
    struct mainloop_source* s;
    assert(m);

    s = malloc(sizeof(struct mainloop_source));
    assert(s);
    memset(s, 0, sizeof(struct mainloop_source));

    s->type = type;
    s->mainloop = m;

    l = get_source_list(m, type);
    assert(l);
            
    s->next = l->sources;
    l->sources = s;
    l->n_sources++;
    return s;
}

struct mainloop_source* mainloop_source_new_io(struct mainloop*m, int fd, enum mainloop_io_event event, void (*callback)(struct mainloop_source*s, int fd, enum mainloop_io_event event, void *userdata), void *userdata) {
    struct mainloop_source* s;
    assert(m && fd>=0 && callback);

    s = source_new(m, MAINLOOP_SOURCE_TYPE_IO);

    s->io.fd = fd;
    s->io.events = event;
    s->io.callback = callback;
    s->userdata = userdata;
    s->io.pollfd.fd = fd;
    s->io.pollfd.events = (event & MAINLOOP_IO_EVENT_IN ? POLLIN : 0) | (event & MAINLOOP_IO_EVENT_OUT ? POLLOUT : 0);
    s->io.pollfd.revents = 0;

    s->enabled = 1;

    m->rebuild_pollfds = 1;
    return s;
}

struct mainloop_source* mainloop_source_new_prepare(struct mainloop*m, void (*callback)(struct mainloop_source *s, void*userdata), void*userdata) {
    struct mainloop_source* s;
    assert(m && callback);

    s = source_new(m, MAINLOOP_SOURCE_TYPE_PREPARE);

    s->prepare.callback = callback;
    s->userdata = userdata;
    s->enabled = 1;
    return s;
}

struct mainloop_source* mainloop_source_new_idle(struct mainloop*m, void (*callback)(struct mainloop_source *s, void*userdata), void*userdata) {
    struct mainloop_source* s;
    assert(m && callback);

    s = source_new(m, MAINLOOP_SOURCE_TYPE_IDLE);

    s->idle.callback = callback;
    s->userdata = userdata;
    s->enabled = 1;
    return s;
}

struct mainloop_source* mainloop_source_new_signal(struct mainloop*m, int sig, void (*callback)(struct mainloop_source *s, int sig, void*userdata), void*userdata) {
    struct mainloop_source* s;
    struct sigaction save_sa, sa;
    
    assert(m && callback);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_func;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    memset(&save_sa, 0, sizeof(save_sa));

    if (sigaction(sig, &sa, &save_sa) < 0)
        return NULL;
    
    s = source_new(m, MAINLOOP_SOURCE_TYPE_SIGNAL);
    s->signal.sig = sig;
    s->signal.sigaction = save_sa;
    
    s->signal.callback = callback;
    s->userdata = userdata;
    s->enabled = 1;
    return s;
}

void mainloop_source_free(struct mainloop_source*s) {
    struct mainloop_source_list *l;
    assert(s && !s->dead);
    s->dead = 1;

    assert(s->mainloop);
    l = get_source_list(s->mainloop, s->type);
    assert(l);

    l->n_sources--;
    l->dead_sources = 1;

    if (s->type == MAINLOOP_SOURCE_TYPE_IO)
        s->mainloop->rebuild_pollfds = 1;
    else if (s->type == MAINLOOP_SOURCE_TYPE_SIGNAL)
        sigaction(s->signal.sig, &s->signal.sigaction, NULL);
}

void mainloop_source_enable(struct mainloop_source*s, int b) {
    assert(s && !s->dead);

    if (s->type == MAINLOOP_SOURCE_TYPE_IO && ((s->enabled && !b) || (!s->enabled && b))) {
        assert(s->mainloop);
        s->mainloop->rebuild_pollfds = 1;
    }

    s->enabled = b;
}

void mainloop_source_io_set_events(struct mainloop_source*s, enum mainloop_io_event events) {
    assert(s && !s->dead && s->type == MAINLOOP_SOURCE_TYPE_IO);

    if ((s->io.events && !events) || (!s->io.events && events)) {
        assert(s->mainloop);
        s->mainloop->rebuild_pollfds = 1;
    }

    s->io.events = events;
    s->io.pollfd.events = ((events & MAINLOOP_IO_EVENT_IN) ? POLLIN : 0) | ((events & MAINLOOP_IO_EVENT_OUT) ? POLLOUT : 0);
}

struct mainloop *mainloop_source_get_mainloop(struct mainloop_source *s) {
    assert(s);

    return s->mainloop;
}
