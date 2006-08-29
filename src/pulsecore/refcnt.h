#ifndef foopulserefcntfoo
#define foopulserefcntfoo

/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <atomic_ops.h>

#define PA_REFCNT_DECLARE volatile AO_t _ref

#define PA_REFCNT_INIT(p) \
  AO_store_release_write(&(p)->_ref,  1)

#define PA_REFCNT_INC(p) \
  AO_fetch_and_add1_release_write(&(p)->_ref)

#define PA_REFCNT_DEC(p) \
  (AO_fetch_and_sub1_release_write(&(p)->_ref)-1)

#define PA_REFCNT_VALUE(p) \
  AO_load_acquire_read(&(p)->_ref)

#endif
