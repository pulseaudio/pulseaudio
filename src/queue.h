#ifndef fooqueuehfoo
#define fooqueuehfoo

struct pa_queue;

struct pa_queue* pa_queue_new(void);
void pa_queue_free(struct pa_queue* q, void (*destroy)(void *p, void *userdata), void *userdata);
void pa_queue_push(struct pa_queue *q, void *p);
void* pa_queue_pop(struct pa_queue *q);

int pa_queue_is_empty(struct pa_queue *q);

#endif
