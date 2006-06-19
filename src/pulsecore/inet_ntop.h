#ifndef fooinet_ntophfoo
#define fooinet_ntophfoo

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "winsock.h"

const char *inet_ntop(int af, const void *src, char *dst, socklen_t cnt);

#endif
