#ifndef fooprotocolnativehfoo
#define fooprotocolnativehfoo

struct protocol_native;

struct protocol_native* protocol_native(struct socket_server *server);
void protocol_native_free(struct protocol_native *n);

#endif
