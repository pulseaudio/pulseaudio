#ifndef fooclientconfhfoo
#define fooclientconfhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <pulsecore/native-common.h>

/* A structure containing configuration data for PulseAudio clients. */

typedef struct pa_client_conf {
    char *daemon_binary, *extra_arguments, *default_sink, *default_source, *default_server, *cookie_file;
    pa_bool_t autospawn, disable_shm;
    uint8_t cookie[PA_NATIVE_COOKIE_LENGTH];
    pa_bool_t cookie_valid; /* non-zero, when cookie is valid */
    size_t shm_size;
} pa_client_conf;

/* Create a new configuration data object and reset it to defaults */
pa_client_conf *pa_client_conf_new(void);
void pa_client_conf_free(pa_client_conf *c);

/* Load the configuration data from the specified file, overwriting
 * the current settings in *c. When the filename is NULL, the
 * default client configuration file name is used. */
int pa_client_conf_load(pa_client_conf *c, const char *filename);

/* Load the configuration data from the environment of the current
   process, overwriting the current settings in *c. */
int pa_client_conf_env(pa_client_conf *c);

/* Load cookie data from c->cookie_file into c->cookie */
int pa_client_conf_load_cookie(pa_client_conf* c);

#endif
