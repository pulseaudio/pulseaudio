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

#include <assert.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include "cpulimit.h"
#include "mainloop.h"

#ifdef TEST2
#include "mainloop-signal.h"
#endif

static time_t start;

#ifdef TEST2

static void func(struct pa_mainloop_api *m, struct pa_signal_event *e, int sig, void *userdata) {
    time_t now;
    time(&now);
    
    if ((now - start) >= 30) {
        m->quit(m, 1);
        fprintf(stderr, "Test failed\n");
    } else
        raise(SIGUSR1);
}

#endif

int main() {
    struct pa_mainloop *m;
    
    m = pa_mainloop_new();
    assert(m);

    pa_cpu_limit_init(pa_mainloop_get_api(m));

    time(&start);

#ifdef TEST2
    pa_signal_init(pa_mainloop_get_api(m));
    pa_signal_new(SIGUSR1, func, NULL);
    raise(SIGUSR1);
    pa_mainloop_run(m, NULL);
    pa_signal_done();
#else
    for (;;) {
        time_t now;
        time(&now);
        
        if ((now - start) >= 30) {
            fprintf(stderr, "Test failed\n");
            break;
        }
    }
#endif

    pa_cpu_limit_done();
    
    pa_mainloop_free(m);

    return 0;
}
