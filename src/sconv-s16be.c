#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "sconv-s16be.h"

#define INT16_FROM INT16_FROM_BE
#define INT16_TO INT16_TO_BE

#define pa_sconv_s16le_to_float32 pa_sconv_s16be_to_float32
#define pa_sconv_s16le_from_float32 pa_sconv_s16be_from_float32

#include "sconv-s16le.c"
