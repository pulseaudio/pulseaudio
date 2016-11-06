#ifndef fooraopcryptofoo
#define fooraopcryptofoo

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

typedef struct pa_raop_secret pa_raop_secret;

pa_raop_secret* pa_raop_secret_new(void);
void pa_raop_secret_free(pa_raop_secret *s);

char* pa_raop_secret_get_iv(pa_raop_secret *s);
char* pa_raop_secret_get_key(pa_raop_secret *s);

int pa_raop_aes_encrypt(pa_raop_secret *s, uint8_t *data, int len);

#endif
