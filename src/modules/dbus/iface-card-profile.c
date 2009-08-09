/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

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

#include <pulsecore/core-util.h>

#include "iface-card-profile.h"

#define OBJECT_NAME "profile"

struct pa_dbusiface_card_profile {
    pa_card_profile *profile;
    char *path;
};

pa_dbusiface_card_profile *pa_dbusiface_card_profile_new(pa_dbusiface_card *card, pa_card_profile *profile, uint32_t idx) {
    pa_dbusiface_card_profile *p = NULL;

    pa_assert(card);
    pa_assert(profile);

    p = pa_xnew(pa_dbusiface_card_profile, 1);
    p->profile = profile;
    p->path = pa_sprintf_malloc("%s/%s%u", pa_dbusiface_card_get_path(card), OBJECT_NAME, idx);

    return p;
}

void pa_dbusiface_card_profile_free(pa_dbusiface_card_profile *p) {
    pa_assert(p);

    pa_xfree(p->path);
    pa_xfree(p);
}

const char *pa_dbusiface_card_profile_get_path(pa_dbusiface_card_profile *p) {
    pa_assert(p);

    return p->path;
}

const char *pa_dbusiface_card_profile_get_name(pa_dbusiface_card_profile *p) {
    pa_assert(p);

    return p->profile->name;
}
