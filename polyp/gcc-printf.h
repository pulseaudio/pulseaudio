#ifndef foogccprintfhfoo
#define foogccprintfhfoo

#ifdef __GNUC__
#define PA_GCC_PRINTF_ATTR(a,b) __attribute__ ((format (printf, a, b)))
#else
#define PA_GCC_PRINTF_ATTR(a,b)
#endif

#endif
