/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/native-common.h>

#include "core-error.h"

#ifdef HAVE_PTHREAD

static pthread_once_t cstrerror_once = PTHREAD_ONCE_INIT;
static pthread_key_t tlsstr_key;

static void inittls(void) {
    int ret;

    ret = pthread_key_create(&tlsstr_key, pa_xfree);
    if (ret) {
        fprintf(stderr, __FILE__ ": CRITICAL: Unable to allocate TLS key (%d)\n", errno);
        exit(-1);
    }
}

#elif HAVE_WINDOWS_H

static DWORD tlsstr_key = TLS_OUT_OF_INDEXES;
static DWORD monitor_key = TLS_OUT_OF_INDEXES;

static void inittls(void) {
    HANDLE mutex;
    char name[64];

    sprintf(name, "pulse%d", (int)GetCurrentProcessId());

    mutex = CreateMutex(NULL, FALSE, name);
    if (!mutex) {
        fprintf(stderr, __FILE__ ": CRITICAL: Unable to create named mutex (%d)\n", (int)GetLastError());
        exit(-1);
    }

    WaitForSingleObject(mutex, INFINITE);

    if (tlsstr_key == TLS_OUT_OF_INDEXES) {
        tlsstr_key = TlsAlloc();
        monitor_key = TlsAlloc();
        if ((tlsstr_key == TLS_OUT_OF_INDEXES) || (monitor_key == TLS_OUT_OF_INDEXES)) {
            fprintf(stderr, __FILE__ ": CRITICAL: Unable to allocate TLS key (%d)\n", (int)GetLastError());
            exit(-1);
        }
    }

    ReleaseMutex(mutex);

    CloseHandle(mutex);
}

/*
 * This is incredibly brain dead, but this is necessary when dealing with
 * the hell that is Win32.
 */
struct monitor_data {
    HANDLE thread;
    void *data;
};

static DWORD WINAPI monitor_thread(LPVOID param) {
    struct monitor_data *data;

    data = (struct monitor_data*)param;
    assert(data);

    WaitForSingleObject(data->thread, INFINITE);

    CloseHandle(data->thread);
    pa_xfree(data->data);
    pa_xfree(data);

    return 0;
}

static void start_monitor(void) {
    HANDLE thread;
    struct monitor_data *data;

    data = pa_xnew(struct monitor_data, 1);
    assert(data);

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
        GetCurrentProcess(), &data->thread, 0, FALSE, DUPLICATE_SAME_ACCESS);

    thread = CreateThread(NULL, 0, monitor_thread, data, 0, NULL);
    assert(thread);

    TlsSetValue(monitor_key, data);

    CloseHandle(thread);
}

#else

/* Unsafe, but we have no choice */
static char *tlsstr;

#endif

const char* pa_cstrerror(int errnum) {
    const char *origbuf;

#ifdef HAVE_STRERROR_R
    char errbuf[128];
#endif

#ifdef HAVE_PTHREAD
    char *tlsstr;

    pthread_once(&cstrerror_once, inittls);

    tlsstr = pthread_getspecific(tlsstr_key);
#elif defined(HAVE_WINDOWS_H)
    char *tlsstr;
    struct monitor_data *data;

    inittls();

    tlsstr = TlsGetValue(tlsstr_key);
    if (!tlsstr)
        start_monitor();
    data = TlsGetValue(monitor_key);
#endif

    if (tlsstr)
        pa_xfree(tlsstr);

#ifdef HAVE_STRERROR_R

#ifdef __GLIBC__
    origbuf = strerror_r(errnum, errbuf, sizeof(errbuf));
    if (origbuf == NULL)
        origbuf = "";
#else
    if (strerror_r(errnum, errbuf, sizeof(errbuf)) == 0) {
        origbuf = errbuf;
        errbuf[sizeof(errbuf) - 1] = '\0';
    } else
        origbuf = "";
#endif

#else
    /* This might not be thread safe, but we hope for the best */
    origbuf = strerror(errnum);
#endif

    tlsstr = pa_locale_to_utf8(origbuf);
    if (!tlsstr) {
        fprintf(stderr, "Unable to convert, filtering\n");
        tlsstr = pa_utf8_filter(origbuf);
    }

#ifdef HAVE_PTHREAD
    pthread_setspecific(tlsstr_key, tlsstr);
#elif defined(HAVE_WINDOWS_H)
    TlsSetValue(tlsstr_key, tlsstr);
    data->data = tlsstr;
#endif

    return tlsstr;
}
