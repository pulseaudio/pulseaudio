#ifndef foocmdlinehfoo
#define foocmdlinehfoo

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


struct pa_cmdline {
    int daemonize, help, fail, verbose, high_priority, stay_root, version, disallow_module_loading, quit_after_last_client_time;
    char *cli_commands;
};

struct pa_cmdline* pa_cmdline_parse(int argc, char * const argv []);
void pa_cmdline_free(struct pa_cmdline *cmd);

void pa_cmdline_help(const char *argv0);

#endif
