/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include <pulsecore/queue.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

int main(int argc, char *argv[]) {
    pa_queue *q;

    pa_assert_se(q = pa_queue_new());

    pa_assert(pa_queue_isempty(q));

    pa_queue_push(q, (void*) "eins");
    pa_log("%s\n", (char*) pa_queue_pop(q));

    pa_assert(pa_queue_isempty(q));

    pa_queue_push(q, (void*) "zwei");
    pa_queue_push(q, (void*) "drei");
    pa_queue_push(q, (void*) "vier");

    pa_log("%s\n", (char*) pa_queue_pop(q));
    pa_log("%s\n", (char*) pa_queue_pop(q));

    pa_queue_push(q, (void*) "fuenf");

    pa_log("%s\n", (char*) pa_queue_pop(q));
    pa_log("%s\n", (char*) pa_queue_pop(q));

    pa_assert(pa_queue_isempty(q));

    pa_queue_push(q, (void*) "sechs");
    pa_queue_push(q, (void*) "sieben");

    pa_queue_free(q, NULL);

    return 0;
}
