#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "ioline.h"
#include "cli.h"
#include "module.h"
#include "sink.h"
#include "source.h"
#include "client.h"

struct cli {
    struct core *core;
    struct ioline *line;

    void (*eof_callback)(struct cli *c, void *userdata);
    void *userdata;
};

static void line_callback(struct ioline *line, const char *s, void *userdata);

struct cli* cli_new(struct core *core, struct iochannel *io) {
    struct cli *c;
    assert(io);

    c = malloc(sizeof(struct cli));
    assert(c);
    c->core = core;
    c->line = ioline_new(io);
    assert(c->line);

    c->userdata = NULL;
    c->eof_callback = NULL;

    ioline_set_callback(c->line, line_callback, c);
    ioline_puts(c->line, "Welcome to polypaudio!\n> ");

    return c;
}

void cli_free(struct cli *c) {
    assert(c);
    ioline_free(c->line);
    free(c);
}

static void line_callback(struct ioline *line, const char *s, void *userdata) {
    struct cli *c = userdata;
    char *t = NULL;
    assert(line && c);

    if (!s) {
        fprintf(stderr, "CLI client exited\n");
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);

        return;
    }

    if (!strcmp(s, "modules"))
        ioline_puts(line, (t = module_list_to_string(c->core)));
    else if (!strcmp(s, "sources"))
        ioline_puts(line, (t = source_list_to_string(c->core)));
    else if (!strcmp(s, "sinks"))
        ioline_puts(line, (t = sink_list_to_string(c->core)));
    else if (!strcmp(s, "clients"))
        ioline_puts(line, (t = client_list_to_string(c->core)));
    else if (!strcmp(s, "exit")) {
        assert(c->core && c->core->mainloop);
        mainloop_quit(c->core->mainloop, -1);
    } else if (*s)
        ioline_puts(line, "Unknown command\n");

    free(t);
    ioline_puts(line, "> ");
}

void cli_set_eof_callback(struct cli *c, void (*cb)(struct cli*c, void *userdata), void *userdata) {
    assert(c && cb);
    c->eof_callback = cb;
    c->userdata = userdata;
}
