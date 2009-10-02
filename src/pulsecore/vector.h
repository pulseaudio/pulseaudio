/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <inttypes.h>

/* First, define HAVE_VECTOR if we have the gcc vector extensions at all */
#if defined(__SSE2__)
    /* || defined(__ALTIVEC__)*/
#define HAVE_VECTOR


/* This is supposed to be portable to different SIMD instruction
 * sets. We define vector types for different base types: uint8_t,
 * int16_t, int32_t, float. The vector type is a union. The fields .i,
 * .u, .f are arrays for accessing the separate elements of a
 * vector. .v is a gcc vector type of the right format. .m is the
 * vector in the type the SIMD extenstion specific intrinsics API
 * expects. PA_xxx_VECTOR_SIZE is the size of the
 * entries. PA_xxxx_VECTOR_MAKE constructs a gcc vector variable with
 * the same value in all elements. */

#ifdef __SSE2__

#include <xmmintrin.h>
#include <emmintrin.h>

#define PA_UINT8_VECTOR_SIZE 16
#define PA_INT16_VECTOR_SIZE 8
#define PA_INT32_VECTOR_SIZE 4
#define PA_FLOAT_VECTOR_SIZE 4

#define PA_UINT8_VECTOR_MAKE(x) (pa_v16qi) { x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x }
#define PA_INT16_VECTOR_MAKE(x) (pa_v8hi) { x, x, x, x, x, x, x, x }
#define PA_INT32_VECTOR_MAKE(x) (pa_v4si) { x, x, x, x }
#define PA_FLOAT_VECTOR_MAKE(x) (pa_v4fi) { x, x, x, x }

#endif

/* uint8_t vector */
typedef uint8_t pa_v16qi __attribute__ ((vector_size (PA_UINT8_VECTOR_SIZE * sizeof(uint8_t))));
typedef union pa_uint8_vector {
    uint8_t u[PA_UINT8_VECTOR_SIZE];
    pa_v16qi v;
#ifdef __SSE2__
    __m128i m;
#endif
} pa_uint8_vector_t;

/* int16_t vector*/
typedef int16_t pa_v8hi __attribute__ ((vector_size (PA_INT16_VECTOR_SIZE * sizeof(int16_t))));
typedef union pa_int16_vector {
    int16_t i[PA_INT16_VECTOR_SIZE];
    pa_v8hi v;
#ifdef __SSE2__
    __m128i m;
#endif
} pa_int16_vector_t;

/* int32_t vector */
typedef int32_t pa_v4si __attribute__ ((vector_size (PA_INT32_VECTOR_SIZE * sizeof(int32_t))));
typedef union pa_int32_vector {
    int32_t i[PA_INT32_VECTOR_SIZE];
    pa_v4si v;
#ifdef __SSE2__
    __m128i m;
#endif
} pa_int32_vector_t;

/* float vector */
typedef float pa_v4sf __attribute__ ((vector_size (PA_FLOAT_VECTOR_SIZE * sizeof(float))));
typedef union pa_float_vector {
    float f[PA_FLOAT_VECTOR_SIZE];
    pa_v4sf v;
#ifdef __SSE2__
    __m128 m;
#endif
} pa_float_vector_t;

#endif
