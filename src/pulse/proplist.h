#ifndef foopulseproplisthfoo
#define foopulseproplisthfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

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

#include <pulsecore/macro.h>

/* Defined properties:
 *
 *    x11.xid
 *    x11.display
 *    x11.x_pointer
 *    x11.y_pointer
 *    x11.button
 *    media.name
 *    media.title
 *    media.artist
 *    media.language
 *    media.filename
 *    media.icon
 *    media.icon_name
 *    media.role                    video, music, game, event, phone, production
 *    application.name
 *    application.version
 *    application.icon
 *    application.icon_name
 */

#define PA_PROP_X11_XID                  "x11.xid"
#define PA_PROP_X11_DISPLAY              "x11.display"
#define PA_PROP_X11_X_POINTER            "x11.x_pointer"
#define PA_PROP_X11_Y_POINTER            "x11.y_pointer"
#define PA_PROP_X11_BUTTON               "x11.button"
#define PA_PROP_MEDIA_NAME               "media.name"
#define PA_PROP_MEDIA_TITLE              "media.title"
#define PA_PROP_MEDIA_ARTIST             "media.artist"
#define PA_PROP_MEDIA_LANGUAGE           "media.language"
#define PA_PROP_MEDIA_FILENAME           "media.filename"
#define PA_PROP_MEDIA_ICON               "media.icon"
#define PA_PROP_MEDIA_ICON_NAME          "media.icon_name"
#define PA_PROP_MEDIA_ROLE               "media.role"
#define PA_PROP_APPLICATION_NAME         "application.name"
#define PA_PROP_APPLICATION_VERSION      "application.version"
#define PA_PROP_APPLICATION_ICON         "application.icon"
#define PA_PROP_APPLICATION_ICON_NAME    "application.icon_name"

typedef struct pa_proplist pa_proplist;

pa_proplist* pa_proplist_new(void);
void pa_proplist_free(pa_proplist* p);

/** Will accept only valid UTF-8 */
int pa_proplist_puts(pa_proplist *p, const char *key, const char *value);
int pa_proplist_put(pa_proplist *p, const char *key, const void *data, size_t nbytes);

/* Will return NULL if the data is not valid UTF-8 */
const char *pa_proplist_gets(pa_proplist *p, const char *key);
int pa_proplist_get(pa_proplist *p, const char *key, const void **data, size_t *nbytes);

void pa_proplist_merge(pa_proplist *p, pa_proplist *other);
int pa_proplist_remove(pa_proplist *p, const char *key);

const char *pa_proplist_iterate(pa_proplist *p, void **state);

char *pa_proplist_to_string(pa_proplist *p);

int pa_proplist_contains(pa_proplist *p, const char *key);

#endif
