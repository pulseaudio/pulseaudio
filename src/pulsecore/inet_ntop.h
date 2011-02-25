#ifndef fooinet_ntophfoo
#define fooinet_ntophfoo

#ifndef HAVE_INET_NTOP

#include <pulsecore/socket.h>

const char *inet_ntop(int af, const void *src, char *dst, socklen_t cnt);

#endif

#endif
