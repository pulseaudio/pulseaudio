#ifndef foopulsertsighfoo
#define foopulsertsighfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

/* Return the next unused POSIX Realtime signals */
int pa_rtsig_get(void);

/* If not called before in the current thread, return the next unused
 * rtsig, and install it in a TLS region and give it up automatically
 * when the thread shuts down */
int pa_rtsig_get_for_thread(void);

/* Give an rtsig back. */
void pa_rtsig_put(int sig);

/* Block all RT signals */
void pa_rtsig_configure(int start, int end);

#endif
