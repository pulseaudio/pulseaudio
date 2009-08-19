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

#include <stdint.h>

#include <pulsecore/log.h>

#include "cpu-arm.h"

static pa_cpu_arm_flag_t pa_cpu_arm_flags;

void pa_cpu_init_arm (void) {
#if defined (__arm__)
    pa_cpu_arm_flags = 0;
   
    pa_log ("ARM init\n");

    pa_volume_func_init_arm (pa_cpu_arm_flags);
#endif /* defined (__arm__) */
}
