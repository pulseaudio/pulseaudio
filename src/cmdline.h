#ifndef foocmdlinehfoo
#define foocmdlinehfoo

struct pa_cmdline_module {
    char *name, *arguments;
    struct pa_cmdline_module *next;
};

struct pa_cmdline {
    int daemonize, help;
    struct pa_cmdline_module *first_module, *last_module;
};

struct pa_cmdline* pa_cmdline_parse(int argc, char * const argv []);
void pa_cmdline_free(struct pa_cmdline *cmd);

void pa_cmdline_help(const char *argv0);

#endif
