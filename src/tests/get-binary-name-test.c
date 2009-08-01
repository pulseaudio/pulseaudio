/***
  This file is part of PulseAudio.

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

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <pulse/util.h>
#include <pulse/xmalloc.h>

int main(int argc, char *argv[]) {
    char *exename;
    size_t allocated = 128;

    for (;;) {
        exename = pa_xmalloc(allocated);

        if (!pa_get_binary_name(exename, allocated)) {
            printf("failed to read binary name\n");
            pa_xfree(exename);
            break;
        }

        if (strlen(exename) < allocated - 1) {
            printf("%s\n", exename);
            pa_xfree(exename);
            break;
        }

        pa_xfree(exename);
        allocated *= 2;
    }

    return 0;
}
