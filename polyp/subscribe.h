#ifndef foosubscribehfoo
#define foosubscribehfoo

#include "core.h"

enum pa_subscription_mask {
    PA_SUBSCRIPTION_FACILITY_SINK = 1,
    PA_SUBSCRIPTION_FACILITY_SOURCE = 2,
    PA_SUBSCRIPTION_FACILITY_SINK_INPUT = 4,
    PA_SUBSCRIPTION_FACILITY_SOURCE_OUTPUT = 8,
    PA_SUBSCRIPTION_FACILITY_MODULE = 16,
    PA_SUBSCRIPTION_FACILITY_CLIENT = 32,
    PA_SUBSCRIPTION_FACILITY_SAMPLE_CACHE = 64,
};

enum pa_subscription_event_type {
    PA_SUBSCRIPTION_EVENT_SINK = 0,
    PA_SUBSCRIPTION_EVENT_SOURCE = 1,
    PA_SUBSCRIPTION_EVENT_SINK_INPUT = 2,
    PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT = 3,
    PA_SUBSCRIPTION_EVENT_MODULE = 4,
    PA_SUBSCRIPTION_EVENT_CLIENT = 5,
    PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE = 6,
    PA_SUBSCRIPTION_EVENT_FACILITY_MASK = 7,

    PA_SUBSCRIPTION_EVENT_NEW = 0,
    PA_SUBSCRIPTION_EVENT_CHANGE = 16,
    PA_SUBSCRIPTION_EVENT_REMOVE = 32,
    PA_SUBSCRIPTION_EVENT_TYPE_MASK = 16+32,
};
    
struct pa_subscription;
struct pa_subscription_event;

struct pa_subscription* pa_subscription_new(struct pa_core *c, enum pa_subscription_mask m,  void (*callback)(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata);
void pa_subscription_free(struct pa_subscription*s);
void pa_subscription_free_all(struct pa_core *c);

void pa_subscription_post(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index);

int pa_subscription_match_flags(enum pa_subscription_mask m, enum pa_subscription_event_type e);

#endif
