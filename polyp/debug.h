#ifndef foodebughfoo
#define foodebughfoo

/* A nice trick for debuggers, working on x86 only */

#define DEBUG_TRAP __asm__("int $3")

#endif
