#ifndef foowinsockhfoo
#define foowinsockhfoo

#ifdef HAVE_WINSOCK2_H
#include <winsock2.h>

#define EBADF           WSAEBADF
#define ESHUTDOWN       WSAESHUTDOWN
#define ECONNRESET      WSAECONNRESET
#define ECONNABORTED    WSAECONNABORTED
#define ENETRESET       WSAENETRESET
#define EINPROGRESS     WSAEINPROGRESS
#define ETIMEDOUT       WSAETIMEDOUT
#define ECONNREFUSED    WSAECONNREFUSED
#define EHOSTUNREACH    WSAEHOSTUNREACH

#endif

#ifdef HAVE_WS2TCPIP_H
#include <ws2tcpip.h>
#endif

#endif
