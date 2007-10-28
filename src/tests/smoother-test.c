/* $Id$ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <stdio.h>
#include <stdlib.h>

#include <pulsecore/time-smoother.h>
#include <pulse/timeval.h>

int main(int argc, char*argv[]) {
    pa_usec_t x;
    unsigned u = 0;
    pa_smoother *s;
    int m;

/*     unsigned msec[] = { */
/*         200, 200, */
/*         300, 320, */
/*         400, 400, */
/*         500, 480, */
/*         0, 0 */
/*     }; */

    int msec[200];

    srand(0);

    for (m = 0, u = 0; u < PA_ELEMENTSOF(msec)-2; u+= 2) {

        msec[u] = m+1;
        msec[u+1] = m + rand() % 2000 - 1000;

        m += rand() % 100;

        if (msec[u+1] < 0)
            msec[u+1] = 0;
    }

    msec[PA_ELEMENTSOF(msec)-2] = 0;
    msec[PA_ELEMENTSOF(msec)-1] = 0;

    s = pa_smoother_new(1000*PA_USEC_PER_MSEC, 2000*PA_USEC_PER_MSEC, TRUE);

    for (x = 0, u = 0; x < PA_USEC_PER_SEC * 10; x += PA_USEC_PER_MSEC) {

        while (msec[u] > 0 && (pa_usec_t) msec[u]*PA_USEC_PER_MSEC < x) {
            pa_smoother_put(s, msec[u]*PA_USEC_PER_MSEC, msec[u+1]*PA_USEC_PER_MSEC);
            printf("%i\t\t%i\n", msec[u],  msec[u+1]);
            u += 2;
        }

        printf("%llu\t%llu\n", x/PA_USEC_PER_MSEC, pa_smoother_get(s, x)/PA_USEC_PER_MSEC);
    }

    pa_smoother_free(s);

    return 0;
}
