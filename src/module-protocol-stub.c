#include <assert.h>
#include <arpa/inet.h>

#include "module.h"
#include "socket-server.h"

#ifdef USE_PROTOCOL_SIMPLE
  #include "protocol-simple.h"
  #define protocol_free protcol_simple_free
#else
  #ifdef USE_PROTOCOL_CLI
    #include "protocol-cli.h" 
    #define protocol_new protocol_cli_new
    #define protocol_free protocol_cli_free
  #else
    #error "Broken build system"
  #endif
#endif

int module_init(struct core *c, struct module*m) {
    struct socket_server *s;
    assert(c && m);

#ifdef USE_TCP_SOCKETS
    if (!(s = socket_server_new_ipv4(c->mainloop, INADDR_LOOPBACK, 4712)))
        return -1;
#else
    if (!(s = socket_server_new_unix(c->mainloop, "/tmp/polypsimple")))
        return -1;
#endif

#ifdef USE_PROTOCOL_SIMPLE
    m->userdata = protocol_simple_new(c, s, PROTOCOL_SIMPLE_PLAYBACK);
#else
    m->userdata = protocol_new(c, s);
#endif
    
    assert(m->userdata);
    return 0;
}

void module_done(struct core *c, struct module*m) {
    assert(c && m);

    protocol_simple_free(m->userdata);
}
