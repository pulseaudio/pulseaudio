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

#include <pulsecore/hook-list.h>

void pa_hook_init(pa_hook *hook, void *data) {
    assert(hook);

    PA_LLIST_HEAD_INIT(pa_hook_slot, hook->slots);
    hook->last = NULL;
    hook->n_dead = hook->firing = 0;
    hook->data = data;
}

static void slot_free(pa_hook *hook, pa_hook_slot *slot) {
    assert(hook);
    assert(slot);

    if (hook->last == slot)
        hook->last = slot->prev;

    PA_LLIST_REMOVE(pa_hook_slot, hook->slots, slot);

    pa_xfree(slot);
}

void pa_hook_free(pa_hook *hook) {
    assert(hook);
    assert(!hook->firing);

    while (hook->slots)
        slot_free(hook, hook->slots);

    pa_hook_init(hook, NULL);
}

pa_hook_slot* pa_hook_connect(pa_hook *hook, pa_hook_cb_t cb, void *data) {
    pa_hook_slot *slot;

    assert(cb);

    slot = pa_xnew(pa_hook_slot, 1);
    slot->hook = hook;
    slot->dead = 0;
    slot->callback = cb;
    slot->data = data;

    PA_LLIST_INSERT_AFTER(pa_hook_slot, hook->slots, hook->last, slot);
    hook->last = slot;

    return slot;
}

void pa_hook_slot_free(pa_hook_slot *slot) {
    assert(slot);
    assert(!slot->dead);

    if (slot->hook->firing > 0) {
        slot->dead = 1;
        slot->hook->n_dead++;
    } else
        slot_free(slot->hook, slot);
}

pa_hook_result_t pa_hook_fire(pa_hook *hook, void *data) {
    pa_hook_slot *slot, *next;
    pa_hook_result_t result = PA_HOOK_OK;

    assert(hook);

    hook->firing ++;

    for (slot = hook->slots; slot; slot = slot->next) {
        if (slot->dead)
            continue;

        if ((result = slot->callback(hook->data, data, slot->data)) != PA_HOOK_OK)
            break;
    }

    hook->firing --;

    for (slot = hook->slots; hook->n_dead > 0 && slot; slot = next) {
        next = slot->next;

        if (slot->dead) {
            slot_free(hook, slot);
            hook->n_dead--;
        }
    }

    return result;
}

