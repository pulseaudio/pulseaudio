#ifndef foomainloophfoo
#define foomainloophfoo

struct mainloop;
struct mainloop_source;

enum mainloop_io_event {
    MAINLOOP_IO_EVENT_NULL = 0,
    MAINLOOP_IO_EVENT_IN = 1,
    MAINLOOP_IO_EVENT_OUT = 2,
    MAINLOOP_IO_EVENT_BOTH = 3
};

enum mainloop_source_type {
    MAINLOOP_SOURCE_TYPE_IO,
    MAINLOOP_SOURCE_TYPE_PREPARE,
    MAINLOOP_SOURCE_TYPE_IDLE,
    MAINLOOP_SOURCE_TYPE_SIGNAL
};

struct mainloop *mainloop_new(void);
void mainloop_free(struct mainloop* m);

int mainloop_iterate(struct mainloop *m, int block);
int mainloop_run(struct mainloop *m);
void mainloop_quit(struct mainloop *m, int r);

struct mainloop_source* mainloop_source_new_io(struct mainloop*m, int fd, enum mainloop_io_event event, void (*callback)(struct mainloop_source*s, int fd, enum mainloop_io_event event, void *userdata), void *userdata);
struct mainloop_source* mainloop_source_new_prepare(struct mainloop*m, void (*callback)(struct mainloop_source *s, void*userdata), void*userdata);
struct mainloop_source* mainloop_source_new_idle(struct mainloop*m, void (*callback)(struct mainloop_source *s, void*userdata), void*userdata);
struct mainloop_source* mainloop_source_new_signal(struct mainloop*m, int sig, void (*callback)(struct mainloop_source *s, int sig, void*userdata), void*userdata);

void mainloop_source_free(struct mainloop_source*s);
void mainloop_source_enable(struct mainloop_source*s, int b);

void mainloop_source_io_set_events(struct mainloop_source*s, enum mainloop_io_event event);

struct mainloop *mainloop_source_get_mainloop(struct mainloop_source *s);

#endif
