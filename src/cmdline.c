#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

#include "cmdline.h"
#include "util.h"

void pa_cmdline_help(const char *argv0) {
    const char *e;

    if ((e = strrchr(argv0, '/')))
        e++;
    else
        e = argv0;
    
    printf("%s [options]\n"
           "  -L MODULE  Load the specified plugin module with the specified argument\n"
           "  -F FILE    A shortcut for '-L module-cli file=FILE', i.e. run the specified script after startup\n"
           "  -C         A shortcut for '-L module-cli', i.e. open a command line on the running TTY\n"
           "  -D         Daemonize after loading the modules\n"
           "  -h         Show this help\n", e);
}

static void add_module(struct pa_cmdline *cmdline, char *name, char *arguments) {
    struct pa_cmdline_module *m;
    assert(cmdline && name);

    m = malloc(sizeof(struct pa_cmdline_module));
    assert(m);
    m->name = name;
    m->arguments = name;
    m->next = NULL;

    if (cmdline->last_module)
        cmdline->last_module->next = m;
    else {
        assert(!cmdline->first_module);
        cmdline->first_module = m;
    }
    cmdline->last_module = m;
}

struct pa_cmdline* pa_cmdline_parse(int argc, char * const argv []) {
    char c;
    struct pa_cmdline *cmdline = NULL;
    assert(argc && argv);

    cmdline = malloc(sizeof(struct pa_cmdline));
    assert(cmdline);
    cmdline->daemonize = cmdline->help = 0;
    cmdline->first_module = cmdline->last_module = NULL;
    
    while ((c = getopt(argc, argv, "L:F:CDh")) != -1) {
        switch (c) {
            case 'L': {
                char *space;
                if ((space = strchr(optarg, ' ')))
                    add_module(cmdline, strndup(optarg, space-optarg), space+1);
                else
                    add_module(cmdline, strdup(optarg), NULL);
                break;
            }
            case 'F':
                add_module(cmdline, strdup("module-cli"), pa_sprintf_malloc("file='%s'", optarg));
                break;
            case 'C':
                add_module(cmdline, strdup("module-cli"), NULL);
                break;
            case 'D':
                cmdline->daemonize = 1;
                break;
            case 'h':
                cmdline->help = 1;
                break;
            default:
                goto fail;
        }
    }

    return cmdline;
    
fail:
    if (cmdline)
        pa_cmdline_free(cmdline);
    return NULL;
}

void pa_cmdline_free(struct pa_cmdline *cmd) {
    struct pa_cmdline_module *m;
    assert(cmd);

    while ((m = cmd->first_module)) {
        cmd->first_module = m->next;
        free(m->name);
        free(m->arguments);
        free(m);
    }
        
    free(cmd);
}
