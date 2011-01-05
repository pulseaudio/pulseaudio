#ifndef fooinet_ptonhfoo
#define fooinet_ptonhfoo

#ifndef HAVE_INET_PTON

#include <pulsecore/socket.h>

int inet_pton(int af, const char *src, void *dst);

#endif

#endif
