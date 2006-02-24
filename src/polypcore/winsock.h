#ifndef foowinsockhfoo
#define foowinsockhfoo

#ifdef HAVE_WINSOCK2_H
#include <winsock2.h>

#define ESHUTDOWN       WSAESHUTDOWN
#define ECONNRESET      WSAECONNRESET
#define ECONNABORTED    WSAECONNABORTED
#define ENETRESET       WSAENETRESET
#define EINPROGRESS     WSAEINPROGRESS
#define EAFNOSUPPORT    WSAEAFNOSUPPORT
#define ETIMEDOUT       WSAETIMEDOUT
#define ECONNREFUSED    WSAECONNREFUSED
#define EHOSTUNREACH    WSAEHOSTUNREACH

#endif

#ifdef HAVE_WS2TCPIP_H
#include <ws2tcpip.h>
static const struct in6_addr in6addr_any = {{ IN6ADDR_ANY_INIT }};
static const struct in6_addr in6addr_loopback = {{ IN6ADDR_LOOPBACK_INIT }};
#endif

#endif
