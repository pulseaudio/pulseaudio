#ifndef fooendianmacroshfoo
#define fooendianmacroshfoo

#include <inttypes.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define INT16_SWAP(x) ((int16_t)(((int16_t) x >> 8) | ((int16_t) x << 8)))
#define UINT16_SWAP(x) ((uint16_t)(((uint16_t) x >> 8) | ((uint16_t) x << 8)))
#define INT32_SWAP(x) ((int32_t)(((int32_t) x >> 24) | ((int32_t) x << 24) | (((int32_t) x & 0xFF00) << 16) | (((int32_t) x) >> 16) & 0xFF00))
#define UINT32_SWAP(x) ((uint32_t)(((uint32_t) x >> 24) | ((uint32_t) x << 24) | (((uint32_t) x & 0xFF00) << 16) | (((uint32_t) x) >> 16) & 0xFF00))

#ifdef WORDS_BIGENDIAN
 #define INT16_FROM_LE(x) INT16_SWAP(x)
 #define INT16_FROM_BE(x) ((int16_t)(x))
 #define INT16_TO_LE(x) INT16_SWAP(x)
 #define INT16_TO_BE(x) ((int16_t)(x))

 #define UINT16_FROM_LE(x) UINT16_SWAP(x)
 #define UINT16_FROM_BE(x) ((uint16_t)(x))
 #define INT32_FROM_LE(x) INT32_SWAP(x)
 #define INT32_FROM_BE(x) ((int32_t)(x))
 #define UINT32_FROM_LE(x) UINT32_SWAP(x)
 #define UINT32_FROM_BE(x) ((uint32_t)(x))
#else
 #define INT16_FROM_LE(x) ((int16_t)(x))
 #define INT16_FROM_BE(x) INT16_SWAP(x)
 #define INT16_TO_LE(x) ((int16_t)(x))
 #define INT16_TO_BE(x) INT16_SWAP(x)

 #define UINT16_FROM_LE(x) ((uint16_t)(x))
 #define UINT16_FROM_BE(x) UINT16_SWAP(x)
 #define INT32_FROM_LE(x) ((int32_t)(x))
 #define INT32_FROM_BE(x) INT32_SWAP(x)
 #define UINT32_FROM_LE(x) ((uint32_t)(x))
 #define UINT32_FROM_BE(x) UINT32_SWAP(x)
#endif

#endif
