/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "util.h"
#include "log.h"
#include "authkey.h"
#include "native-common.h"
#include "client-conf.h"

static void set_x11_prop(Display *d, const char *name, const char *data) {
    Atom a = XInternAtom(d, name, False);
    XChangeProperty(d, RootWindow(d, 0), a, XA_STRING, 8, PropModeReplace, (unsigned char*) data, strlen(data)+1);
}

static void del_x11_prop(Display *d, const char *name) {
    Atom a = XInternAtom(d, name, False);
    XDeleteProperty(d, RootWindow(d, 0), a);
}

static char* get_x11_prop(Display *d, const char *name, char *p, size_t l) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long nbytes_after;
    unsigned char *prop = NULL;
    char *ret = NULL;
    
    Atom a = XInternAtom(d, name, False);
    if (XGetWindowProperty(d, RootWindow(d, 0), a, 0, (l+2)/4, False, XA_STRING, &actual_type, &actual_format, &nitems, &nbytes_after, &prop) != Success)
        goto finish;

    if (actual_type != XA_STRING)
        goto finish;

    memcpy(p, prop, nitems);
    p[nitems] = 0;

    ret = p;

finish:

    if (prop)
        XFree(prop);
    
    return ret;
}

int main(int argc, char *argv[]) {
    const char *dname = NULL, *sink = NULL, *source = NULL, *server = NULL, *cookie_file = PA_NATIVE_COOKIE_FILE;
    int c, ret = 1;
    Display *d = NULL;
    enum { DUMP, EXPORT, IMPORT, REMOVE } mode = DUMP;

    while ((c = getopt(argc, argv, "deiD:S:O:I:c:hr")) != -1) {
        switch (c) {
            case 'D' :
                dname = optarg;
                break;
            case 'h':
                printf("%s [-D display] [-S server] [-O sink] [-I source] [-c file]  [-d|-e|-i|-r]\n\n"
                       " -d    Show current Polypaudio data attached to X11 display (default)\n"
                       " -e    Export local Polypaudio data to X11 display\n"
                       " -i    Import Polypaudio data from X11 display to local environment variables and cookie file.\n"
                       " -r    Remove Polypaudio data from X11 display\n",
                       pa_path_get_filename(argv[0]));
                ret = 0;
                goto finish;
            case 'd':
                mode = DUMP;
                break;
            case 'e':
                mode = EXPORT;
                break;
            case 'i':
                mode = IMPORT;
                break;
            case 'r':
                mode = REMOVE;
                break;
            case 'c':
                cookie_file = optarg;
                break;
            case 'I':
                source = optarg;
                break;
            case 'O':
                sink = optarg;
                break;
            case 'S':
                server = optarg;
                break;
            default:
                fprintf(stderr, "Failed to parse command line.\n");
                goto finish;
        }
    }

    if (!(d = XOpenDisplay(dname))) {
        pa_log(__FILE__": XOpenDisplay() failed\n");
        goto finish;
    }

    switch (mode) {
        case DUMP: {
            char t[1024];
            if (!get_x11_prop(d, "POLYP_SERVER", t, sizeof(t))) 
                goto finish;

            printf("Server: %s\n", t);
            if (get_x11_prop(d, "POLYP_SOURCE", t, sizeof(t)))
                printf("Source: %s\n", t);
            if (get_x11_prop(d, "POLYP_SINK", t, sizeof(t)))
                printf("Sink: %s\n", t);
            if (get_x11_prop(d, "POLYP_COOKIE", t, sizeof(t)))
                printf("Cookie: %s\n", t);

            break;
        }
            
        case IMPORT: {
            char t[1024];
            if (!get_x11_prop(d, "POLYP_SERVER", t, sizeof(t))) 
                goto finish;

            printf("POLYP_SERVER='%s'\nexport POLYP_SERVER\n", t);
            
            if (get_x11_prop(d, "POLYP_SOURCE", t, sizeof(t)))
                printf("POLYP_SOURCE='%s'\nexport POLYP_SOURCE\n", t);
            if (get_x11_prop(d, "POLYP_SINK", t, sizeof(t)))
                printf("POLYP_SINK='%s'\nexport POLYP_SINK\n", t);

            if (get_x11_prop(d, "POLYP_COOKIE", t, sizeof(t))) {
                uint8_t cookie[PA_NATIVE_COOKIE_LENGTH];
                size_t l;
                if ((l = pa_parsehex(t, cookie, sizeof(cookie))) == (size_t) -1) {
                    fprintf(stderr, "Failed to parse cookie data\n");
                    goto finish;
                }

                if (pa_authkey_save(cookie_file, cookie, l) < 0) {
                    fprintf(stderr, "Failed to save cookie data\n");
                    goto finish;
                }
            }

            break;
        }

        case EXPORT: {
            struct pa_client_conf *c = pa_client_conf_new();
            uint8_t cookie[PA_NATIVE_COOKIE_LENGTH];
            char hx[PA_NATIVE_COOKIE_LENGTH*2+1];
            assert(c);

            if (pa_client_conf_load(c, NULL) < 0) {
                fprintf(stderr, "Failed to load client configuration file.\n");
                goto finish;
            }

            if (pa_client_conf_env(c) < 0) {
                fprintf(stderr, "Failed to read environment configuration data.\n");
                goto finish;
            }

            del_x11_prop(d, "POLYP_ID");

            if (server)
                set_x11_prop(d, "POLYP_SERVER", c->default_server);
            else if (c->default_server)
                set_x11_prop(d, "POLYP_SERVER", c->default_server);
            else {
                char hn[256];
                if (!pa_get_fqdn(hn, sizeof(hn))) {
                    fprintf(stderr, "Failed to get FQDN.\n");
                    goto finish;
                }
                    
                set_x11_prop(d, "POLYP_SERVER", hn);
            }

            if (sink)
                set_x11_prop(d, "POLYP_SINK", sink);
            else if (c->default_sink)
                set_x11_prop(d, "POLYP_SINK", c->default_sink);

            if (source)
                set_x11_prop(d, "POLYP_SOURCE", source);
            if (c->default_source)
                set_x11_prop(d, "POLYP_SOURCE", c->default_source);

            pa_client_conf_free(c);
            
            if (pa_authkey_load_auto(cookie_file, cookie, sizeof(cookie)) < 0) {
                fprintf(stderr, "Failed to load cookie data\n");
                goto finish;
            }

            set_x11_prop(d, "POLYP_COOKIE", pa_hexstr(cookie, sizeof(cookie), hx, sizeof(hx)));
            break;
        }

        case REMOVE:
            del_x11_prop(d, "POLYP_SERVER");
            del_x11_prop(d, "POLYP_SINK");
            del_x11_prop(d, "POLYP_SOURCE");
            del_x11_prop(d, "POLYP_ID");
            del_x11_prop(d, "POLYP_COOKIE");
            break;
            
        default:
            fprintf(stderr, "No yet implemented.\n");
            goto finish;
    }

    ret = 0;
    
finish:

    if (d) {
        XSync(d, False);
        XCloseDisplay(d);
    }
    
    return ret;
}
