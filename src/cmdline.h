#ifndef foocmdlinehfoo
#define foocmdlinehfoo


struct pa_cmdline {
    int daemonize, help, fail, verbose;
    char *cli_commands;
};

struct pa_cmdline* pa_cmdline_parse(int argc, char * const argv []);
void pa_cmdline_free(struct pa_cmdline *cmd);

void pa_cmdline_help(const char *argv0);

#endif
