#ifndef foohooklistfoo
#define foohooklistfoo

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

/* Some macro voodoo to implement a type safe hook list */

#include <pulsecore/llist.h>
#include <pulse/xmalloc.h>
#include <pulse/cdecl.h>

typedef enum pa_hook_result {
    PA_HOOK_OK = 0,
    PA_HOOK_STOP = 1,
    PA_HOOK_CANCEL = -1
} pa_hook_result_t;

#define PA_HOOK_DECLARE(name, arg1, arg2) \
typedef pa_hook_result_t (*pa_hook__##name##__func_t)(arg1 a, arg2 b, void *userdata); \
\
typedef struct pa_hook__##name##__func_info pa_hook__##name##__func_info; \
struct pa_hook__##name##__func_info { \
    int dead; \
    pa_hook__##name##__func_t func; \
    void *userdata; \
    PA_LLIST_FIELDS(pa_hook__##name##__func_info); \
}; \
PA_GCC_UNUSED static void pa_hook__##name##__free_one( \
        pa_hook__##name##__func_info **head, \
        pa_hook__##name##__func_info *i) { \
    PA_LLIST_REMOVE(pa_hook__##name##__func_info, *head, i); \
    pa_xfree(i); \
} \
PA_GCC_UNUSED static void pa_hook__##name##__free_all( \
        pa_hook__##name##__func_info **head) { \
    while (*head) \
        pa_hook__##name##__free_one(head, *head); \
} \
PA_GCC_UNUSED static void pa_hook__##name##__mark_dead( \
        pa_hook__##name##__func_info *i, \
        pa_hook__##name##__func_t func, \
        void *userdata) { \
    for (; i; i = i->next) { \
        if (i->func != func || i->userdata != userdata) \
            continue; \
        i->dead = 1; \
        break; \
    } \
} \
PA_GCC_UNUSED static void pa_hook__##name##__append( \
        pa_hook__##name##__func_info **head, \
        pa_hook__##name##__func_t func, \
        void *userdata) { \
    pa_hook__##name##__func_info *i = pa_xnew(pa_hook__##name##__func_info, 1); \
    i->dead = 0; \
    i->func = func; \
    i->userdata = userdata; \
    PA_LLIST_PREPEND(pa_hook__##name##__func_info, *head, i); \
} \
PA_GCC_UNUSED static pa_hook_result_t pa_hook__##name##__execute ( \
        pa_hook__##name##__func_info **head, \
        arg1 a, \
        arg2 b) { \
    pa_hook__##name##__func_info *i, *n; \
    pa_hook_result_t ret = PA_HOOK_OK; \
    for (i = *head; i; i = i->next) { \
        if ((ret = i->func(a, b, i->userdata)) != PA_HOOK_OK) \
            break; \
    } \
    for (i = *head; i; i = n) { \
        n = i->next; \
        if (i->dead) \
            pa_hook__##name##__free_one(head, i); \
    } \
    return ret; \
}\
void pa_hook__##name##__nowarn(void)


#define PA_HOOK_HEAD(name, head) \
pa_hook__##name##__func_info *head;

#define PA_HOOK_HEAD_INIT(name, head) \
(head) = NULL

#define PA_HOOK_EXECUTE(name, head, arg1, arg2) \
pa_hook__##name##__execute(&(head), arg1, arg2)

#define PA_HOOK_APPEND(name, head, func, userdata) \
pa_hook__##name##__append(&(head), func, userdata)

#define PA_HOOK_REMOVE(name, head, func, userdata) \
pa_hook__##name##__mark_dead(head, func, userdata)

#define PA_HOOK_FREE(name, head) \
pa_hook__##name##__free_all(&(head))

#endif
