#ifndef fooraopsinkfoo
#define fooraopsinkfoo

/***
  This file is part of PulseAudio.

  Copyright 2013 Martin Blanchard

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

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/sink.h>

pa_sink* pa_raop_sink_new(pa_module *m, pa_modargs *ma, const char *driver);

void pa_raop_sink_free(pa_sink *s);

#endif
