/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include <pulse/i18n.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "caps.h"

/* Glibc <= 2.2 has broken unistd.h */
#if defined(linux) && (__GLIBC__ <= 2 && __GLIBC_MINOR__ <= 2)
int setresgid(gid_t r, gid_t e, gid_t s);
int setresuid(uid_t r, uid_t e, uid_t s);
#endif

#ifdef HAVE_GETUID

/* Drop root rights when called SUID root */
void pa_drop_root(void) {
    uid_t uid = getuid();

    if (uid == 0 || geteuid() != 0)
        return;

    pa_log_info(_("Dropping root privileges."));

#if defined(HAVE_SETRESUID)
    pa_assert_se(setresuid(uid, uid, uid) >= 0);
#elif defined(HAVE_SETREUID)
    pa_assert_se(setreuid(uid, uid) >= 0);
#else
    pa_assert_se(setuid(uid) >= 0);
    pa_assert_se(seteuid(uid) >= 0);
#endif

    pa_assert_se(getuid() == uid);
    pa_assert_se(geteuid() == uid);
}

#else

void pa_drop_root(void) {
}

#endif

#if defined(HAVE_SYS_CAPABILITY_H) && defined(HAVE_SYS_PRCTL_H)

/* Limit permitted capabilities set to CAPSYS_NICE */
void pa_limit_caps(void) {
    cap_t caps;
    cap_value_t nice_cap = CAP_SYS_NICE;

    pa_assert_se(caps = cap_init());
    pa_assert_se(cap_clear(caps) == 0);
    pa_assert_se(cap_set_flag(caps, CAP_EFFECTIVE, 1, &nice_cap, CAP_SET) == 0);
    pa_assert_se(cap_set_flag(caps, CAP_PERMITTED, 1, &nice_cap, CAP_SET) == 0);

    if (cap_set_proc(caps) < 0)
        /* Hmm, so we couldn't limit our caps, which probably means we
         * hadn't any in the first place, so let's just make sure of
         * that */
        pa_drop_caps();
    else
        pa_log_info(_("Limited capabilities successfully to CAP_SYS_NICE."));

    pa_assert_se(cap_free(caps) == 0);

    pa_assert_se(prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == 0);
}

/* Drop all capabilities, effectively becoming a normal user */
void pa_drop_caps(void) {
    cap_t caps;

#ifndef __OPTIMIZE__
    /* Valgrind doesn't not know set_caps, so we bypass it here -- but
     * only in development builds.*/

    if (pa_in_valgrind() && !pa_have_caps())
        return;
#endif

    pa_assert_se(prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) == 0);

    pa_assert_se(caps = cap_init());
    pa_assert_se(cap_clear(caps) == 0);
    pa_assert_se(cap_set_proc(caps) == 0);
    pa_assert_se(cap_free(caps) == 0);

    pa_assert_se(!pa_have_caps());
}

pa_bool_t pa_have_caps(void) {
    cap_t caps;
    cap_flag_value_t flag = CAP_CLEAR;

#ifdef __OPTIMIZE__
    pa_assert_se(caps = cap_get_proc());
#else
    if (!(caps = cap_get_proc()))
        return FALSE;
#endif
    pa_assert_se(cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &flag) >= 0);
    pa_assert_se(cap_free(caps) == 0);

    return flag == CAP_SET;
}

#else

/* NOOPs in case capabilities are not available. */
void pa_limit_caps(void) {
}

void pa_drop_caps(void) {
    pa_drop_root();
}

pa_bool_t pa_have_caps(void) {
    return FALSE;
}

#endif
