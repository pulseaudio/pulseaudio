#ifndef fooclicommandhfoo
#define fooclicommandhfoo

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

#include "strbuf.h"
#include "core.h"

int pa_cli_command_execute_line(struct pa_core *c, const char *s, struct pa_strbuf *buf, int *fail, int *verbose);
int pa_cli_command_execute_file(struct pa_core *c, const char *fn, struct pa_strbuf *buf, int *fail, int *verbose);
int pa_cli_command_execute(struct pa_core *c, const char *s, struct pa_strbuf *buf, int *fail, int *verbose);

#endif
