#ifndef foopulsecoresockethfoo
#define foopulsecoresockethfoo

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_WINSOCK2_H
#include <winsock2.h>
#include "winerrno.h"

typedef long suseconds_t;

/** Windows 10 supports AF_UNIX as of build 17603, with
    support provided in the header file <afunix.h>.  However,
    only the latest Windows SDK provides this file; older SDKs and
    MinGW do not.

    Hence we define SOCKADDR_UN here.  We do not expect this definition to change
    as Windows has some pretty good binary backwards-compatibility guarantees.

    This shouldn't pose a problem for older versions of Windows; we expect them to
    fail with an error whenever we try to make a socket of type AF_UNIX. */
#define UNIX_PATH_MAX 108

typedef struct sockaddr_un
{
     ADDRESS_FAMILY sun_family;     /* AF_UNIX */
     char sun_path[UNIX_PATH_MAX];  /* pathname */
} SOCKADDR_UN, *PSOCKADDR_UN;

#ifndef SUN_LEN
#define SUN_LEN(ptr) \
    ((size_t)(((struct sockaddr_un *) 0)->sun_path) + strlen((ptr)->sun_path))
#endif

#define HAVE_SYS_UN_H

#endif

#ifdef HAVE_WS2TCPIP_H
#include <ws2tcpip.h>
#endif

#endif
