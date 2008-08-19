/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>

#include <pulsecore/macro.h>
#include <pulsecore/flist.h>
#include <pulsecore/once.h>
#include <pulsecore/thread.h>
#include <pulsecore/core-util.h>

#include "rtsig.h"

#ifdef SIGRTMIN

static void _free_rtsig(void *p) {
    pa_rtsig_put(PA_PTR_TO_INT(p));
}

PA_STATIC_FLIST_DECLARE(rtsig_flist, pa_make_power_of_two((unsigned) (SIGRTMAX-SIGRTMIN+1)), NULL);
PA_STATIC_TLS_DECLARE(rtsig_tls, _free_rtsig);

static pa_atomic_t rtsig_current = PA_ATOMIC_INIT(-1);

static int rtsig_start = -1, rtsig_end = -1;

int pa_rtsig_get(void) {
    void *p;
    int sig;

    if ((p = pa_flist_pop(PA_STATIC_FLIST_GET(rtsig_flist))))
        return PA_PTR_TO_INT(p);

    sig = pa_atomic_dec(&rtsig_current);

    pa_assert(sig <= SIGRTMAX);
    pa_assert(sig <= rtsig_end);

    if (sig < rtsig_start) {
        pa_atomic_inc(&rtsig_current);
        return -1;
    }

    return sig;
}

int pa_rtsig_get_for_thread(void) {
    int sig;
    void *p;

    if ((p = PA_STATIC_TLS_GET(rtsig_tls)))
        return PA_PTR_TO_INT(p);

    if ((sig = pa_rtsig_get()) < 0)
        return -1;

    PA_STATIC_TLS_SET(rtsig_tls, PA_INT_TO_PTR(sig));
    return sig;
}

void pa_rtsig_put(int sig) {
    pa_assert(sig >= rtsig_start);
    pa_assert(sig <= rtsig_end);

    pa_assert_se(pa_flist_push(PA_STATIC_FLIST_GET(rtsig_flist), PA_INT_TO_PTR(sig)) >= 0);
}

void pa_rtsig_configure(int start, int end) {
    int s;
    sigset_t ss;

    pa_assert(pa_atomic_load(&rtsig_current) == -1);

    pa_assert(SIGRTMIN <= start);
    pa_assert(start <= end);
    pa_assert(end <= SIGRTMAX);

    rtsig_start = start;
    rtsig_end = end;

    sigemptyset(&ss);

    for (s = rtsig_start; s <= rtsig_end; s++)
        pa_assert_se(sigaddset(&ss, s) == 0);

    pa_assert(pthread_sigmask(SIG_BLOCK, &ss, NULL) == 0);

    /* We allocate starting from the end */
    pa_atomic_store(&rtsig_current, rtsig_end);
}

#else /* SIGRTMIN */

int pa_rtsig_get(void) {
    return -1;
}

int pa_rtsig_get_for_thread(void) {
    return -1;
}

void pa_rtsig_put(int sig) {
}

void pa_rtsig_configure(int start, int end) {
}

#endif /* SIGRTMIN */
