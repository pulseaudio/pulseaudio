#include <assert.h>
#include <stdlib.h>
#include "mainloop-api.h"

struct once_info {
    void (*callback)(void *userdata);
    void *userdata;
};

static void once_callback(struct pa_mainloop_api *api, void *id, void *userdata) {
    struct once_info *i = userdata;
    assert(api && i && i->callback);
    i->callback(i->userdata);
    assert(api->cancel_fixed);
    api->cancel_fixed(api, id);
    free(i);
}

void pa_mainloop_api_once(struct pa_mainloop_api* api, void (*callback)(void *userdata), void *userdata) {
    struct once_info *i;
    void *id;
    assert(api && callback);

    i = malloc(sizeof(struct once_info));
    assert(i);
    i->callback = callback;
    i->userdata = userdata;

    assert(api->source_fixed);
    id = api->source_fixed(api, once_callback, i);
    assert(id);

    /* Note: if the mainloop is destroyed before once_callback() was called, some memory is leaked. */
}

