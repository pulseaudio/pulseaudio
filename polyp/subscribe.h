#ifndef foosubscribehfoo
#define foosubscribehfoo

#include "core.h"
#include "native-common.h"

struct pa_subscription;
struct pa_subscription_event;

struct pa_subscription* pa_subscription_new(struct pa_core *c, enum pa_subscription_mask m,  void (*callback)(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata);
void pa_subscription_free(struct pa_subscription*s);
void pa_subscription_free_all(struct pa_core *c);

void pa_subscription_post(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index);

#endif
