/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk>

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include "cpu-arm.h"

#include "sample-util.h"

#if defined (__arm__) && defined (HAVE_ARMV6)

#define MOD_INC() \
    " subs  r0, r6, %2              \n\t" \
    " itt cs                        \n\t" \
    " addcs r0, %1                  \n\t" \
    " movcs r6, r0                  \n\t"

static void pa_volume_s16ne_arm(int16_t *samples, const int32_t *volumes, unsigned channels, unsigned length) {
    /* Channels must be at least 4, and always a multiple of the original number.
     * This is also the max amount we overread the volume array, which should
     * have enough padding. */
    const int32_t *ve = volumes + (channels == 3 ? 6 : PA_MAX (4U, channels));

    __asm__ __volatile__ (
        " mov r6, %1                      \n\t" /* r6 = volumes */
        " mov %3, %3, LSR #1              \n\t" /* length /= sizeof (int16_t) */
        " tst %3, #1                      \n\t" /* check for odd samples */
        " beq  2f                         \n\t"

        "1:                               \n\t" /* odd samples volumes */
        " ldr  r0, [r6], #4               \n\t" /* r0 = volume */
        " ldrh r2, [%0]                   \n\t" /* r2 = sample */

        " smulwb r0, r0, r2               \n\t" /* r0 = (r0 * r2) >> 16 */
        " ssat r0, #16, r0                \n\t" /* r0 = PA_CLAMP(r0, 0x7FFF) */

        " strh r0, [%0], #2               \n\t" /* sample = r0 */

        MOD_INC()

        "2:                               \n\t"
        " mov %3, %3, LSR #1              \n\t"
        " tst %3, #1                      \n\t" /* check for odd samples */
        " beq  4f                         \n\t"

        "3:                               \n\t"
        " ldrd r2, [r6], #8               \n\t" /* 2 samples at a time */
        " ldr  r0, [%0]                   \n\t"

#ifdef WORDS_BIGENDIAN
        " smulwt r2, r2, r0               \n\t"
        " smulwb r3, r3, r0               \n\t"
#else
        " smulwb r2, r2, r0               \n\t"
        " smulwt r3, r3, r0               \n\t"
#endif

        " ssat r2, #16, r2                \n\t"
        " ssat r3, #16, r3                \n\t"

#ifdef WORDS_BIGENDIAN
        " pkhbt r0, r3, r2, LSL #16       \n\t"
#else
        " pkhbt r0, r2, r3, LSL #16       \n\t"
#endif
        " str  r0, [%0], #4               \n\t"

        MOD_INC()

        "4:                               \n\t"
        " movs %3, %3, LSR #1             \n\t"
        " beq  6f                         \n\t"

        "5:                               \n\t"
        " ldrd r2, [r6], #8               \n\t" /* 4 samples at a time */
        " ldrd r4, [r6], #8               \n\t"
        " ldrd r0, [%0]                   \n\t"

#ifdef WORDS_BIGENDIAN
        " smulwt r2, r2, r0               \n\t"
        " smulwb r3, r3, r0               \n\t"
        " smulwt r4, r4, r1               \n\t"
        " smulwb r5, r5, r1               \n\t"
#else
        " smulwb r2, r2, r0               \n\t"
        " smulwt r3, r3, r0               \n\t"
        " smulwb r4, r4, r1               \n\t"
        " smulwt r5, r5, r1               \n\t"
#endif

        " ssat r2, #16, r2                \n\t"
        " ssat r3, #16, r3                \n\t"
        " ssat r4, #16, r4                \n\t"
        " ssat r5, #16, r5                \n\t"

#ifdef WORDS_BIGENDIAN
        " pkhbt r0, r3, r2, LSL #16       \n\t"
        " pkhbt r1, r5, r4, LSL #16       \n\t"
#else
        " pkhbt r0, r2, r3, LSL #16       \n\t"
        " pkhbt r1, r4, r5, LSL #16       \n\t"
#endif
        " strd  r0, [%0], #8              \n\t"

        MOD_INC()

        " subs %3, %3, #1                 \n\t"
        " bne 5b                          \n\t"
        "6:                               \n\t"

        : "+r" (samples), "+r" (volumes), "+r" (ve), "+r" (length)
        :
        : "r6", "r5", "r4", "r3", "r2", "r1", "r0", "cc"
    );
}

#endif /* defined (__arm__) && defined (HAVE_ARMV6) */

void pa_volume_func_init_arm(pa_cpu_arm_flag_t flags) {
#if defined (__arm__) && defined (HAVE_ARMV6)
    pa_log_info("Initialising ARM optimized volume functions.");

    pa_set_volume_func(PA_SAMPLE_S16NE, (pa_do_volume_func_t) pa_volume_s16ne_arm);
#endif /* defined (__arm__) && defined (HAVE_ARMV6) */
}
