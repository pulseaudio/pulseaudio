#ifndef fooqueuehfoo
#define fooqueuehfoo

struct queue;

struct queue* queue_new(void);
void queue_free(struct queue* q, void (*destroy)(void *p, void *userdata), void *userdata);
void queue_push(struct queue *q, void *p);
void* queue_pop(struct queue *q);

int queue_is_empty(struct queue *q);

#endif
