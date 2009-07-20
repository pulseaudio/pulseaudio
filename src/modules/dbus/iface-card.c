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

#include "iface-card.h"

#define OBJECT_NAME "card"

struct pa_dbusiface_card {
    pa_card *card;
    char *path;
};

pa_dbusiface_card *pa_dbusiface_card_new(pa_card *card, const char *path_prefix) {
    pa_dbusiface_card *c;

    pa_assert(card);
    pa_assert(path_prefix);

    c = pa_xnew(pa_dbusiface_card, 1);
    c->card = card;
    c->path = pa_sprintf_malloc("%s/%s%u", path_prefix, OBJECT_NAME, card->index);

    return c;
}

void pa_dbusiface_card_free(pa_dbusiface_card *c) {
    pa_assert(c);

    pa_xfree(c->path);
    pa_xfree(c);
}

const char *pa_dbusiface_card_get_path(pa_dbusiface_card *c) {
    pa_assert(c);

    return c->path;
}
