/* $Id$ */

#include <pulsecore/hook-list.h>
#include <pulsecore/log.h>

static pa_hook_result_t func1(const char*a, void *userdata) {
    pa_log("#1 arg=%s userdata=%s", a, (char*) userdata);
    return PA_HOOK_OK;
}

static pa_hook_result_t func2(const char*a, void *userdata) {
    pa_log("#2 arg=%s userdata=%s", a, (char*) userdata);
    return PA_HOOK_OK;
}

int main(int argc, char *argv[]) {
    pa_hook hook;
    pa_hook_slot *slot;

    pa_hook_init(&hook);

    pa_hook_connect(&hook, (pa_hook_cb_t) func1, (void*) "1-1");
    slot = pa_hook_connect(&hook, (pa_hook_cb_t) func2, (void*) "2-1");
    pa_hook_connect(&hook, (pa_hook_cb_t) func1, (void*) "1-2");
    
    pa_hook_fire(&hook, (void*) "arg2");

    pa_hook_slot_free(slot);
    pa_hook_free(&hook);
    
    return 0;
}
