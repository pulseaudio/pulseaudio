#ifndef foollistfoo
#define foollistfoo

#define PA_LLIST_HEAD(t,name) t *name

#define PA_LLIST_FIELDS(t) t *next, *prev;

#define PA_LLIST_HEAD_INIT(t,item) do { (item) = NULL; } while(0)

#define PA_LLIST_INIT(t,item) do { \
                               t *_item = (item); \
                               assert(_item); \
                               _item->prev = _item->next = NULL; \
                               } while(0)

#define PA_LLIST_PREPEND(t,head,item) do { \
                                        t **_head = &(head), *_item = (item); \
                                        assert(_item); \
                                        if ((_item->next = *_head)) \
                                           _item->next->prev = _item; \
                                        _item->prev = NULL; \
                                        *_head = _item; \
                                        } while (0)

#define PA_LLIST_REMOVE(t,head,item) do { \
                                    t **_head = &(head), *_item = (item); \
                                    assert(_item); \
                                    if (_item->next) \
                                       _item->next->prev = _item->prev; \
                                    if (_item->prev) \
                                       _item->prev->next = _item->next; \
                                    else {\
                                       assert(*_head == _item); \
                                       *_head = _item->next; \
                                    } \
                                    _item->next = _item->prev = NULL; \
                                    } while(0)

#endif
