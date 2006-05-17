#ifndef foopolypaudiohfoo
#define foopolypaudiohfoo

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

#include <polyp/mainloop-api.h>
#include <polyp/sample.h>
#include <polyp/def.h>
#include <polyp/context.h>
#include <polyp/stream.h>
#include <polyp/introspect.h>
#include <polyp/subscribe.h>
#include <polyp/scache.h>
#include <polyp/version.h>
#include <polyp/error.h>
#include <polyp/operation.h>
#include <polyp/channelmap.h>
#include <polyp/volume.h>

/** \file
 * Include all polyplib header file at once. The following
 * files are included: \ref mainloop-api.h, \ref sample.h, \ref def.h,
 * \ref context.h, \ref stream.h, \ref introspect.h, \ref subscribe.h,
 * \ref scache.h, \ref version.h, \ref error.h, \ref channelmap.h,
 * \ref operation.h and \ref volume.h at once */

/** \mainpage
 *
 * \section intro_sec Introduction
 * 
 * This document describes the client API for the polypaudio sound
 * server. The API comes in two flavours to accomodate different styles
 * of applications and different needs in complexity:
 * 
 * \li The complete but somewhat complicated to use asynchronous API
 * \li The simplified, easy to use, but limited synchronous API
 *
 * \section simple_sec Simple API
 *
 * Use this if you develop your program in synchronous style and just
 * need a way to play or record data on the sound server. See
 * \subpage simple for more details.
 *
 * \section async_sec Asynchronous API
 *
 * Use this if you develop your programs in asynchronous, event loop
 * based style or if you want to use the advanced features of the
 * polypaudio API. A guide can be found in \subpage async.
 *
 * By using the built-in threaded main loop, it is possible to acheive a
 * pseudo-synchronous API, which can be useful in synchronous applications
 * where the simple API is insufficient. See the \ref async page for
 * details.
 *
 * \section thread_sec Threads
 *
 * The polypaudio client libraries are not designed to be used in a
 * heavily threaded environment. They are however designed to be reentrant
 * safe.
 *
 * To use a the libraries in a threaded environment, you must assure that
 * all objects are only used in one thread at a time. Normally, this means
 * that all objects belonging to a single context must be accessed from the
 * same thread.
 *
 * The included main loop implementation is also not thread safe. Take care
 * to make sure event lists are not manipulated when any other code is
 * using the main loop.
 *
 * \section pkgconfig pkg-config
 *
 * The polypaudio libraries provide pkg-config snippets for the different
 * modules:
 *
 * \li polyplib - The asynchronous API and the internal main loop
 *                implementation.
 * \li polyplib-glib12-mainloop - GLIB 1.2 main loop bindings.
 * \li polyplib-glib-mainloop - GLIB 2.x main loop bindings.
 * \li polyplib-simple - The simple polypaudio API.
 */

#endif
