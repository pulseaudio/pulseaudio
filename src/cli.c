#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

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
#include "sink-input.h"
#include "source-output.h"
#include "tokenizer.h"
#include "strbuf.h"
#include "namereg.h"
#include "clitext.h"
#include "cli-command.h"

struct pa_cli {
    struct pa_core *core;
    struct pa_ioline *line;

    void (*eof_callback)(struct pa_cli *c, void *userdata);
    void *userdata;

    struct pa_client *client;

    int fail, verbose, kill_requested, defer_kill;
};

static void line_callback(struct pa_ioline *line, const char *s, void *userdata);

static const char prompt[] = ">>> ";

static void client_kill(struct pa_client *c);

struct pa_cli* pa_cli_new(struct pa_core *core, struct pa_iochannel *io, struct pa_module *m) {
    char cname[256];
    struct pa_cli *c;
    assert(io);

    c = malloc(sizeof(struct pa_cli));
    assert(c);
    c->core = core;
    c->line = pa_ioline_new(io);
    assert(c->line);

    c->userdata = NULL;
    c->eof_callback = NULL;

    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    c->client = pa_client_new(core, "CLI", cname);
    assert(c->client);
    c->client->kill = client_kill;
    c->client->userdata = c;
    c->client->owner = m;
    
    pa_ioline_set_callback(c->line, line_callback, c);
    pa_ioline_puts(c->line, "Welcome to polypaudio! Use \"help\" for usage information.\n");
    pa_ioline_puts(c->line, prompt);

    c->fail = c->kill_requested = c->defer_kill = 0;
    c->verbose = 1;
    
    return c;
}

void pa_cli_free(struct pa_cli *c) {
    assert(c);
    pa_ioline_free(c->line);
    pa_client_free(c->client);
    free(c);
}

static void client_kill(struct pa_client *client) {
    struct pa_cli *c;
    assert(client && client->userdata);
    c = client->userdata;
    
    fprintf(stderr, "CLI client killed.\n");
    if (c->defer_kill)
        c->kill_requested = 1;
    else {
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);
    }
}

static void line_callback(struct pa_ioline *line, const char *s, void *userdata) {
    struct pa_strbuf *buf;
    struct pa_cli *c = userdata;
    char *p;
    assert(line && c);

    if (!s) {
        fprintf(stderr, "CLI got EOF from user.\n");
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);

        return;
    }

    buf = pa_strbuf_new();
    assert(buf);
    c->defer_kill++;
    pa_cli_command_execute_line(c->core, s, buf, &c->fail, &c->verbose);
    c->defer_kill--;
    pa_ioline_puts(line, p = pa_strbuf_tostring_free(buf));
    free(p);

    if (c->kill_requested) {
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);
    } else    
        pa_ioline_puts(line, prompt);
}

void pa_cli_set_eof_callback(struct pa_cli *c, void (*cb)(struct pa_cli*c, void *userdata), void *userdata) {
    assert(c && cb);
    c->eof_callback = cb;
    c->userdata = userdata;
}
