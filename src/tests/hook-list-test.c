#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/hook-list.h>
#include <pulsecore/log.h>

static pa_hook_result_t func1(const char *hook_data, const char *call_data, const char *slot_data) {
    pa_log("(func1) hook=%s call=%s slot=%s", hook_data, call_data, slot_data);
    return PA_HOOK_OK;
}

static pa_hook_result_t func2(const char *hook_data, const char *call_data, const char *slot_data) {
    pa_log("(func2) hook=%s call=%s slot=%s", hook_data, call_data, slot_data);
    return PA_HOOK_OK;
}

int main(int argc, char *argv[]) {
    pa_hook hook;
    pa_hook_slot *slot;

    pa_hook_init(&hook, (void*) "hook");

    pa_hook_connect(&hook, PA_HOOK_LATE, (pa_hook_cb_t) func1, (void*) "slot1");
    slot = pa_hook_connect(&hook, PA_HOOK_NORMAL, (pa_hook_cb_t) func2, (void*) "slot2");
    pa_hook_connect(&hook, PA_HOOK_NORMAL, (pa_hook_cb_t) func1, (void*) "slot3");

    pa_hook_fire(&hook, (void*) "call1");

    pa_hook_slot_free(slot);

    pa_hook_fire(&hook, (void*) "call2");

    pa_hook_done(&hook);

    return 0;
}
