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

#include <assert.h>
#include <inttypes.h>

#include "endianmacros.h"
#include "sconv.h"

#ifndef INT16_FROM
#define INT16_FROM INT16_FROM_LE
#endif

#ifndef INT16_TO
#define INT16_TO INT16_TO_LE
#endif

void pa_sconv_s16le_to_float32(unsigned n, const void *a, unsigned an, float *b) {
    const int16_t *ca = a;
    assert(n && a && an && b);

    for (; n > 0; n--) {
        unsigned i;
        float sum = 0;
        
        for (i = 0; i < an; i++) {
            int16_t s = *(ca++);
            sum += ((float) INT16_FROM(s))/0x7FFF;
        }

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;
        
        *(b++) = sum;
    }
}

void pa_sconv_s16le_from_float32(unsigned n, const float *a, void *b, unsigned bn) {
    int16_t *cb = b;
    assert(n && a && b && bn);
    
    for (; n > 0; n--) {
        unsigned i;
        int16_t s;
        float v = *(a++);

        if (v > 1)
            v = 1;
        if (v < -1)
            v = -1;

        s = (int16_t) (v * 0x7FFF);
        s = INT16_TO(s);

        for (i = 0; i < bn; i++)
            *(cb++) = s;
    }
}
