#ifndef foopulseproplisthfoo
#define foopulseproplisthfoo

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

#include <sys/types.h>

#include <pulse/cdecl.h>
#include <pulse/gccmacro.h>

PA_C_DECL_BEGIN

/* Defined properties:
 *
 *    media.name                    "Guns'N'Roses: Civil War"
 *    media.title                   "Civil War"
 *    media.artist                  "Guns'N'Roses"
 *    media.language                "de_DE"
 *    media.filename
 *    media.icon                    Binary blob containing PNG icon data
 *    media.icon_name               Name from XDG icon naming spec
 *    media.role                    video, music, game, event, phone, production, filter, abstract, stream
 *    event.id                      Name from XDG sound naming spec
 *    event.description             "Button blabla clicked" for a11y
 *    event.mouse.x
 *    event.mouse.y
 *    event.mouse.hpos              Float formatted as string in range 0..1
 *    event.mouse.vpos              Float formatted as string in range 0..1
 *    event.mouse.button            Button number following X11 ordering
 *    window.name
 *    window.id                     "org.gnome.rhytmbox.MainWindow"
 *    window.icon                   Binary blob containing PNG icon data
 *    window.icon_name              Name from XDG icon naming spec
 *    window.x11.display
 *    window.x11.screen
 *    window.x11.monitor
 *    window.x11.xid
 *    application.name              "Rhythmbox Media Player"
 *    application.id                "org.gnome.rhythmbox"
 *    application.version
 *    application.icon              Binary blob containing PNG icon data
 *    application.icon_name         Name from XDG icon naming spec
 *    application.language
 *    application.process.id
 *    application.process.binary
 *    application.process.user
 *    application.process.host
 *    device.string
 *    device.api                    oss, alsa, sunaudio
 *    device.description
 *    device.bus_path
 *    device.serial
 *    device.vendor_product_id
 *    device.class                  sound, modem, monitor, filter, abstract
 *    device.form_factor            laptop-speakers, external-speakers, telephone, tv-capture, webcam-capture, microphone-capture, headset
 *    device.connector              isa, pci, usb, firewire, bluetooth
 *    device.access_mode            mmap, mmap_rewrite, serial
 *    device.master_device
 *    device.bufferin.buffer_size
 *    device.bufferin.fragment_size
 */
#define PA_PROP_MEDIA_NAME                     "media.name"
#define PA_PROP_MEDIA_TITLE                    "media.title"
#define PA_PROP_MEDIA_ARTIST                   "media.artist"
#define PA_PROP_MEDIA_LANGUAGE                 "media.language"
#define PA_PROP_MEDIA_FILENAME                 "media.filename"
#define PA_PROP_MEDIA_ICON                     "media.icon"
#define PA_PROP_MEDIA_ICON_NAME                "media.icon_name"
#define PA_PROP_MEDIA_ROLE                     "media.role"
#define PA_PROP_EVENT_ID                       "event.id"
#define PA_PROP_EVENT_DESCRIPTION              "event.description"
#define PA_PROP_EVENT_MOUSE_X                  "event.mouse.x"
#define PA_PROP_EVENT_MOUSE_Y                  "event.mouse.y"
#define PA_PROP_EVENT_MOUSE_HPOS               "event.mouse.hpos"
#define PA_PROP_EVENT_MOUSE_VPOS               "event.mouse.vpos"
#define PA_PROP_EVENT_MOUSE_BUTTON             "event.mouse.button"
#define PA_PROP_WINDOW_NAME                    "window.name"
#define PA_PROP_WINDOW_ID                      "window.id"
#define PA_PROP_WINDOW_ICON                    "window.icon"
#define PA_PROP_WINDOW_ICON_NAME               "window.icon_name"
#define PA_PROP_WINDOW_X11_DISPLAY             "window.x11.display"
#define PA_PROP_WINDOW_X11_SCREEN              "window.x11.screen"
#define PA_PROP_WINDOW_X11_MONITOR             "window.x11.monitor"
#define PA_PROP_WINDOW_X11_XID                 "window.x11.xid"
#define PA_PROP_APPLICATION_NAME               "application.name"
#define PA_PROP_APPLICATION_ID                 "application.id"
#define PA_PROP_APPLICATION_VERSION            "application.version"
#define PA_PROP_APPLICATION_ICON               "application.icon"
#define PA_PROP_APPLICATION_ICON_NAME          "application.icon_name"
#define PA_PROP_APPLICATION_LANGUAGE           "application.language"
#define PA_PROP_APPLICATION_PROCESS_ID         "application.process.id"
#define PA_PROP_APPLICATION_PROCESS_BINARY     "application.process.binary"
#define PA_PROP_APPLICATION_PROCESS_USER       "application.process.user"
#define PA_PROP_APPLICATION_PROCESS_HOST       "application.process.host"
#define PA_PROP_DEVICE_STRING                  "device.string"
#define PA_PROP_DEVICE_API                     "device.api"
#define PA_PROP_DEVICE_DESCRIPTION             "device.description"
#define PA_PROP_DEVICE_BUS_PATH                "device.bus_path"
#define PA_PROP_DEVICE_SERIAL                  "device.serial"
#define PA_PROP_DEVICE_VENDOR_PRODUCT_ID       "device.vendor_product_id"
#define PA_PROP_DEVICE_CLASS                   "device.class"
#define PA_PROP_DEVICE_FORM_FACTOR             "device.form_factor"
#define PA_PROP_DEVICE_CONNECTOR               "device.connector"
#define PA_PROP_DEVICE_ACCESS_MODE             "device.access_mode"
#define PA_PROP_DEVICE_MASTER_DEVICE           "device.master_device"
#define PA_PROP_DEVICE_BUFFERING_BUFFER_SIZE   "device.buffering.buffer_size"
#define PA_PROP_DEVICE_BUFFERING_FRAGMENT_SIZE "device.buffering.fragment_size"

/** A property list object. Basically a dictionary with UTF-8 strings
 * as keys and arbitrary data as values. \since 0.9.11 */
typedef struct pa_proplist pa_proplist;

/** Allocate a property list. \since 0.9.11 */
pa_proplist* pa_proplist_new(void);

/** Free the property list. \since 0.9.11 */
void pa_proplist_free(pa_proplist* p);

/** Append a new string entry to the property list, possibly
 * overwriting an already existing entry with the same key. An
 * internal copy of the data passed is made. Will accept only valid
 * UTF-8. \since 0.9.11 */
int pa_proplist_sets(pa_proplist *p, const char *key, const char *value);

/** Append a new string entry to the property list, possibly
 * overwriting an already existing entry with the same key. An
 * internal copy of the data passed is made. Will accept only valid
 * UTF-8. The data can be passed as printf()-style format string with
 * arguments. \since 0.9.11 */
int pa_proplist_setf(pa_proplist *p, const char *key, const char *format, ...) PA_GCC_PRINTF_ATTR(3,4);

/** Append a new arbitrary data entry to the property list, possibly
 * overwriting an already existing entry with the same key. An
 * internal copy of the data passed is made. \since 0.9.11 */
int pa_proplist_set(pa_proplist *p, const char *key, const void *data, size_t nbytes);

/* Return a string entry for the specified key. Will return NULL if
 * the data is not valid UTF-8. Will return a NUL-terminated string in
 * an internally allocated buffer. The caller should make a copy of
 * the data before accessing the property list again. \since 0.9.11 */
const char *pa_proplist_gets(pa_proplist *p, const char *key);

/** Return the the value for the specified key. Will return a
 * NUL-terminated string for string entries. The pointer returned will
 * point to an internally allocated buffer. The caller should make a
 * copy of the data before the property list is accessed again. \since
 * 0.9.11 */
int pa_proplist_get(pa_proplist *p, const char *key, const void **data, size_t *nbytes);

/** Update mode enum for pa_proplist_update(). \since 0.9.11 */
typedef enum pa_update_mode {
    PA_UPDATE_SET,
    /*< Replace the entirey property list with the new one. Don't keep
     *  any of the old data around */

    PA_UPDATE_MERGE,
    /*< Merge new property list into the existing one, not replacing
     *  any old entries if they share a common key with the new
     *  property list. */

    PA_UPDATE_REPLACE
    /*< Merge new property list into the existing one, replacing all
     *  old entries that share a common key with the new property
     *  list. */
} pa_update_mode_t;

/** Merge property list "other" into "p", adhering the merge mode as
 * specified in "mode". \since 0.9.11 */
void pa_proplist_update(pa_proplist *p, pa_update_mode_t mode, pa_proplist *other);

/** Removes a single entry from the property list, identified be the
 * specified key name. \since 0.9.11 */
int pa_proplist_unset(pa_proplist *p, const char *key);

/** Similar to pa_proplist_remove() but takes an array of keys to
 * remove. The array should be terminated by a NULL pointer. Return -1
 * on failure, otherwise the number of entries actually removed (which
 * might even be 0, if there where no matching entries to
 * remove). \since 0.9.11 */
int pa_proplist_unset_many(pa_proplist *p, const char * const keys[]);

/** Iterate through the property list. The user should allocate a
 * state variable of type void* and initialize it with NULL. A pointer
 * to this variable should then be passed to pa_proplist_iterate()
 * which should be called in a loop until it returns NULL which
 * signifies EOL. The property list should not be modified during
 * iteration through the list -- except for deleting the current
 * looked at entry. On each invication this function will return the
 * key string for the next entry. The keys in the property list do not
 * have any particular order. \since 0.9.11 */
const char *pa_proplist_iterate(pa_proplist *p, void **state);

/** Format the property list nicely as a human readable string. Call pa_xfree() on the result. \since
 * 0.9.11 */
char *pa_proplist_to_string(pa_proplist *p);

/** Returns 1 if an entry for the specified key is existant in the
 * property list. \since 0.9.11 */
int pa_proplist_contains(pa_proplist *p, const char *key);

/** Remove all entries from the property list object. \since 0.9.11 */
void pa_proplist_clear(pa_proplist *p);

/** Allocate a new property list and copy over every single entry from
 * the specific list. \since 0.9.11 */
pa_proplist* pa_proplist_copy(pa_proplist *t);

PA_C_DECL_END

#endif
