#ifndef fooinet_ptonhfoo
#define fooinet_ptonhfoo

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "winsock.h"

int inet_pton(int af, const char *src, void *dst);

#endif
