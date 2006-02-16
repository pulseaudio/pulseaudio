#ifndef foopolyplibhfoo
#define foopolyplibhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <polyp/cdecl.h>
#include <polyp/mainloop-api.h>
#include <polyp/sample.h>
#include <polyp/def.h>
#include <polyp/context.h>
#include <polyp/stream.h>
#include <polyp/introspect.h>
#include <polyp/subscribe.h>
#include <polyp/scache.h>
#include <polyp/version.h>

/** \file
 * Include all polyplib header file at once. The following files are included: \ref mainloop-api.h, \ref sample.h,
 * \ref def.h, \ref context.h, \ref stream.h,
 * \ref introspect.h, \ref subscribe.h and \ref scache.h \ref version.h
 * at once */

/** \mainpage
 *
 * \section intro_sec Introduction
 * 
 * This document describes the client API for the polypaudio sound
 * server. The API comes in two flavours:
 * 
 * \li The complete but somewhat complicated to use asynchronous API
 * \li And the simplified, easy to use, but limited synchronous API
 *
 * The polypaudio client libraries are thread safe as long as all
 * objects created by any library function are accessed from the thread
 * that created them only.
 * 
 * \section simple_sec Simple API
 *
 * Use this if you develop your program in synchronous style and just
 * need a way to play or record data on the sound server. See
 * \ref simple.h for more details.
 *
 * \section async_api Asynchronous API
 *
 * Use this if you develop your programs in asynchronous, main loop
 * based style or want to use advanced features of the polypaudio
 * API. A good starting point is \ref context.h
 *
 * The asynchronous API relies on an abstract main loop API that is
 * described in \ref mainloop-api.h. Two distinct implementations are
 * available:
 * 
 * \li \ref mainloop.h: a minimal but fast implementation based on poll()
 * \li \ref glib-mainloop.h: a wrapper around GLIB's main loop
 *
 * UNIX signals may be hooked to a main loop using the functions from
 * \ref mainloop-signal.h
 *
 * \section pkgconfig pkg-config
 *
 * The polypaudio libraries provide pkg-config snippets for the different modules. To use the
 * asynchronous API use "polyplib" as pkg-config file. GLIB main loop
 * support is available as "glib-mainloop". The simple
 * synchronous API is available as "simple".
 */

#endif
