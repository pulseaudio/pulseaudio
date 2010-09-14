#ifndef foocpuhfoo
#define foocpuhfoo

/***
  This file is part of PulseAudio.

  Copyright 2010 Arun Raghavan

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

#include <pulsecore/cpu-x86.h>
#include <pulsecore/cpu-arm.h>

typedef enum {
    PA_CPU_UNDEFINED = 0,
    PA_CPU_X86,
    PA_CPU_ARM,
} pa_cpu_type_t;

typedef struct pa_cpu_info pa_cpu_info;

struct pa_cpu_info {
    pa_cpu_type_t cpu_type;

    union {
        pa_cpu_x86_flag_t x86;
        pa_cpu_arm_flag_t arm;
    } flags;
};

#endif /* foocpuhfoo */
