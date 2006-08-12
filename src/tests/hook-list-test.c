/* $Id$ */

#include <pulsecore/hook-list.h>
#include <pulsecore/log.h>

PA_HOOK_DECLARE(test, const char *, const char*);
PA_HOOK_IMPLEMENT(test, const char *, const char *);

static pa_hook_result_t func1(const char*a, const char*b, void *userdata) {
    pa_log("#1 a=%s b=%s userdata=%s", a, b, (char*) userdata);
    return PA_HOOK_OK;
}

static pa_hook_result_t func2(const char*a, const char*b, void *userdata) {
    pa_log("#2 a=%s b=%s userdata=%s", a, b, (char*) userdata);
    return PA_HOOK_OK;
}

int main(int argc, char *argv[]) {
    void *u;
    
    PA_HOOK_HEAD(test, test);

    PA_HOOK_HEAD_INIT(test, test);

    PA_HOOK_APPEND(test, test, func1, (void*) "1-1");
    PA_HOOK_APPEND(test, test, func2, u = (void*) "2");
    PA_HOOK_APPEND(test, test, func1, (void*) "1-2");


    PA_HOOK_EXECUTE(test, test, "arg1", "arg2");

    PA_HOOK_REMOVE(test, test, func2, u);

    PA_HOOK_FREE(test, test);
    
    return 0;
}
