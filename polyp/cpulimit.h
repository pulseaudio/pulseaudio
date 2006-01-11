#ifndef foocpulimithfoo
#define foocpulimithfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include "mainloop-api.h"

/* This kills the polypaudio process if it eats more than 70% of the
 * CPU time. This is build around setrlimit() and SIGXCPU. It is handy
 * in case of using SCHED_FIFO which may freeze the whole machine  */

int pa_cpu_limit_init(pa_mainloop_api *m);
void pa_cpu_limit_done(void);

#endif
