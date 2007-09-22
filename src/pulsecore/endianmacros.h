#ifndef fooendianmacroshfoo
#define fooendianmacroshfoo

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

#include <inttypes.h>

#ifndef PACKAGE
#error "Please include config.h before including this file!"
#endif

#ifdef HAVE_BYTESWAP_H
#include <byteswap.h>
#endif

#ifdef HAVE_BYTESWAP_H
#define INT16_SWAP(x) ((int16_t) bswap_16((uint16_t) x))
#define UINT16_SWAP(x) ((uint16_t) bswap_16((uint16_t) x))
#define INT32_SWAP(x) ((int32_t) bswap_32((uint32_t) x))
#define UINT32_SWAP(x) ((uint32_t) bswap_32((uint32_t) x))
#else
#define INT16_SWAP(x) ( (int16_t) ( ((uint16_t) x >> 8) | ((uint16_t) x << 8) ) )
#define UINT16_SWAP(x) ( (uint16_t) ( ((uint16_t) x >> 8) | ((uint16_t) x << 8) ) )
#define INT32_SWAP(x) ( (int32_t) ( ((uint32_t) x >> 24) | ((uint32_t) x << 24) | (((uint32_t) x & 0xFF00) << 8) | ((((uint32_t) x) >> 8) & 0xFF00) ) )
#define UINT32_SWAP(x) ( (uint32_t) ( ((uint32_t) x >> 24) | ((uint32_t) x << 24) | (((uint32_t) x & 0xFF00) << 8) | ((((uint32_t) x) >> 8) & 0xFF00) ) )
#endif

#define MAYBE_INT32_SWAP(c,x) ((c) ? INT32_SWAP(x) : x)
#define MAYBE_UINT32_SWAP(c,x) ((c) ? UINT32_SWAP(x) : x)

#ifdef WORDS_BIGENDIAN
 #define INT16_FROM_LE(x) INT16_SWAP(x)
 #define INT16_FROM_BE(x) ((int16_t)(x))

 #define INT16_TO_LE(x) INT16_SWAP(x)
 #define INT16_TO_BE(x) ((int16_t)(x))

 #define UINT16_FROM_LE(x) UINT16_SWAP(x)
 #define UINT16_FROM_BE(x) ((uint16_t)(x))

 #define INT32_FROM_LE(x) INT32_SWAP(x)
 #define INT32_FROM_BE(x) ((int32_t)(x))

 #define UINT32_FROM_LE(x) UINT32_SWAP(x)
 #define UINT32_FROM_BE(x) ((uint32_t)(x))

 #define UINT32_TO_LE(x) UINT32_SWAP(x)
 #define UINT32_TO_BE(x) ((uint32_t)(x))
#else
 #define INT16_FROM_LE(x) ((int16_t)(x))
 #define INT16_FROM_BE(x) INT16_SWAP(x)

 #define INT16_TO_LE(x) ((int16_t)(x))
 #define INT16_TO_BE(x) INT16_SWAP(x)

 #define UINT16_FROM_LE(x) ((uint16_t)(x))
 #define UINT16_FROM_BE(x) UINT16_SWAP(x)

 #define INT32_FROM_LE(x) ((int32_t)(x))
 #define INT32_FROM_BE(x) INT32_SWAP(x)

 #define UINT32_FROM_LE(x) ((uint32_t)(x))
 #define UINT32_FROM_BE(x) UINT32_SWAP(x)

 #define UINT32_TO_LE(x) ((uint32_t)(x))
 #define UINT32_TO_BE(x) UINT32_SWAP(x)
#endif

#endif
